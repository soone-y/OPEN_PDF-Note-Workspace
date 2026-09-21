#include "ui/menus/menu_build.h"

#include "core/app_core.h"
#include "core/localization.h"
#include "core/path_safety.h"
#include "file_output/file_output.h"
#include "ui/menus/main_debug_menu.h"
#include "ui/menus/main_menu_owner_draw.h"
#include "ui/menus/main_status_display.h"
#include "pdf_view/pdf_view.h"
#include "bridge/view_bridge.h"
#include "settings/settings.h"
#include "workspace/file_ops.h"

#include <windows.h>

namespace {

UINT MenuStringState(bool enabled) {
    return enabled ? MF_STRING : (MF_STRING | MF_GRAYED);
}

} // namespace

HMENU BuildMenuBar() {
    return BuildMenuBarForState(CaptureMainMenuStateSnapshot());
}

HMENU BuildMenuBarForState(const MainMenuStateSnapshot& menuState) {
    HMENU bar = CreateMenu();
    const auto& ui = GetUiText();
    auto text = [](const wchar_t* key) { return localization::Text(key); };

    HMENU file = CreatePopupMenu();
    HMENU create = CreatePopupMenu();
    AppendMenuW(create, MF_STRING, ID_FILE_NEW_CLRO, text(L"menu.file.create_note").c_str());
    AppendMenuW(create, MenuStringState(!g_currentLecturePath.empty()), ID_FILE_NEW_SESSION,
                text(L"menu.file.create_session").c_str());
    AppendMenuW(create, MF_STRING, ID_FILE_NEW_LECTURE, text(L"menu.file.create_lecture").c_str());
    AppendMenuW(file, MF_POPUP, reinterpret_cast<UINT_PTR>(create), text(L"menu.file.create").c_str());

    HMENU import = CreatePopupMenu();
    AppendMenuW(import, MF_STRING, ID_FILE_IMPORT_FILE, text(L"menu.file.import_file").c_str());
    AppendMenuW(import, MenuStringState(!g_currentLecturePath.empty()), ID_FILE_IMPORT_DIR_AS_SESSION,
                text(L"menu.file.import_session").c_str());
    AppendMenuW(import, MF_STRING, ID_FILE_IMPORT_DIR_AS_LECTURE,
                text(L"menu.file.import_lecture").c_str());
    AppendMenuW(file, MF_POPUP, reinterpret_cast<UINT_PTR>(import), text(L"menu.file.import").c_str());

    HMENU explorer = CreatePopupMenu();
    AppendMenuW(explorer, MF_STRING, ID_FILE_OPEN_WORKSPACE_DIR, text(L"menu.file.open_root").c_str());
    AppendMenuW(explorer, MF_STRING, ID_FILE_OPEN_LECTURE_DIR, text(L"menu.file.open_lecture").c_str());
    AppendMenuW(explorer, MF_STRING, ID_FILE_OPEN_SESSION_DIR, text(L"menu.file.open_session").c_str());
    AppendMenuW(file, MF_POPUP, reinterpret_cast<UINT_PTR>(explorer), text(L"menu.file.open_explorer").c_str());

    AppendMenuW(file, MF_STRING, ID_FILE_OPEN_WS, ui.menuOpenWs.c_str());
    AppendMenuW(file, MF_STRING, ID_FILE_RELOAD_WS, ui.menuReloadWs.c_str());

    HMENU temp = CreatePopupMenu();
    AppendMenuW(temp, MF_STRING, ID_FILE_ADD_TEMP_EXTERNAL_LECTURE,
                text(L"menu.file.add_external_folder").c_str());
    AppendMenuW(temp, MF_STRING, ID_FILE_REMOVE_TEMP_EXTERNAL_LECTURE, text(L"menu.common.delete").c_str());
    AppendMenuW(file, MF_POPUP, reinterpret_cast<UINT_PTR>(temp), text(L"menu.file.temporary_paths").c_str());
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, ID_FILE_ORGANIZE_SESSION_FILES, text(L"menu.file.organize").c_str());
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, ID_FILE_EXIT, ui.menuExit.c_str());
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), text(L"menu.bar.file").c_str());

    HMENU edit = CreatePopupMenu();
    AppendMenuW(edit, MenuStringState(menuState.canUndo), ID_EDIT_UNDO,
                localization::Text(L"menu.edit.undo").c_str());
    AppendMenuW(edit, MenuStringState(menuState.canRedo), ID_EDIT_REDO,
                localization::Text(L"menu.edit.redo").c_str());
    UINT renamePdfFlags = !CurrentLogicalPdfPath().empty() ? MF_STRING : (MF_STRING | MF_GRAYED);
    UINT renameNoteFlags = !g_currentNotePath.empty() ? MF_STRING : (MF_STRING | MF_GRAYED);
    HMENU rename = CreatePopupMenu();
    AppendMenuW(rename, renamePdfFlags, ID_OP_RENAME_PDF, text(L"menu.common.pdf").c_str());
    AppendMenuW(rename, renameNoteFlags, ID_OP_RENAME_NOTE, text(L"menu.common.note").c_str());
    AppendMenuW(edit, MF_POPUP, reinterpret_cast<UINT_PTR>(rename), text(L"menu.edit.rename").c_str());
    HMENU move = CreatePopupMenu();
    AppendMenuW(move, renamePdfFlags, ID_OP_MOVE_PDF, text(L"menu.common.pdf").c_str());
    AppendMenuW(move, renameNoteFlags, ID_OP_MOVE_NOTE, text(L"menu.common.note").c_str());
    AppendMenuW(edit, MF_POPUP, reinterpret_cast<UINT_PTR>(move), text(L"menu.edit.move_file").c_str());
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(edit), localization::Text(L"menu.edit.title").c_str());

    HMENU view = CreatePopupMenu();
    const UINT pdfViewFlags = MenuStringState(menuState.hasCurrentPdf);
    HMENU close = CreatePopupMenu();
    AppendMenuW(close, pdfViewFlags, ID_VIEW_CLOSE_PDF, text(L"menu.common.pdf").c_str());
    AppendMenuW(close, MenuStringState(!g_currentNotePath.empty()), ID_VIEW_CLOSE_NOTE,
                text(L"menu.common.note").c_str());
    AppendMenuW(view, MF_POPUP, reinterpret_cast<UINT_PTR>(close), text(L"menu.view.close").c_str());
    HMENU page = CreatePopupMenu();
    AppendMenuW(page, pdfViewFlags, ID_VIEW_FIRST_PAGE, text(L"menu.page.first").c_str());
    AppendMenuW(page, pdfViewFlags, ID_VIEW_PREV_PAGE, text(L"menu.page.previous").c_str());
    AppendMenuW(page, pdfViewFlags, ID_VIEW_NEXT_PAGE, text(L"menu.page.next").c_str());
    AppendMenuW(page, pdfViewFlags, ID_VIEW_LAST_PAGE, text(L"menu.page.last").c_str());
    AppendMenuW(page, pdfViewFlags, ID_VIEW_JUMP_PAGE, text(L"menu.page.jump").c_str());
    AppendMenuW(view, MF_POPUP, reinterpret_cast<UINT_PTR>(page), text(L"menu.view.page").c_str());
    HMENU zoom = CreatePopupMenu();
    AppendMenuW(zoom, pdfViewFlags, ID_VIEW_RESET_ZOOM, text(L"menu.zoom.reset").c_str());
    AppendMenuW(zoom, pdfViewFlags, ID_VIEW_SET_ZOOM, text(L"menu.zoom.set").c_str());
    AppendMenuW(view, MF_POPUP, reinterpret_cast<UINT_PTR>(zoom), text(L"menu.view.zoom").c_str());
    HMENU scroll = CreatePopupMenu();
    HMENU scrollDir = CreatePopupMenu();
    g_hScrollDirectionMenu = scrollDir;
    AppendMenuW(scrollDir, MF_STRING, ID_VIEW_SCROLL_DIR_V_TTB, ui.menuScrollDirVTopToBottom.c_str());
    AppendMenuW(scrollDir, MF_STRING, ID_VIEW_SCROLL_DIR_V_BTU, ui.menuScrollDirVBottomToTop.c_str());
    AppendMenuW(scrollDir, MF_STRING, ID_VIEW_SCROLL_DIR_H_RTL, ui.menuScrollDirHRightToLeft.c_str());
    AppendMenuW(scrollDir, MF_STRING, ID_VIEW_SCROLL_DIR_H_LTR, ui.menuScrollDirHLeftToRight.c_str());
    AppendMenuW(scroll, MF_POPUP, reinterpret_cast<UINT_PTR>(scrollDir), ui.menuScrollDirection.c_str());
    UINT singlePageFlags = MF_STRING;
    if (g_config.pdfSinglePageMode) singlePageFlags |= MF_CHECKED;
    AppendMenuW(scroll, singlePageFlags, ID_VIEW_PDF_SINGLE_PAGE_MODE, text(L"menu.scroll.page_display").c_str());
    AppendMenuW(view, MF_POPUP, reinterpret_cast<UINT_PTR>(scroll), text(L"menu.view.scroll").c_str());
    AppendMenuW(view, MF_STRING | (g_readableTextOverlay ? MF_CHECKED : 0), ID_VIEW_READABLE_TEXT_OVERLAY,
                text(L"menu.view.readability").c_str());
    HMENU bottomPane = CreatePopupMenu();
    g_hBottomPaneMenu = bottomPane;
    AppendMenuW(bottomPane, MF_STRING, ID_VIEW_BOTTOM_NOTE, ui.menuBottomNote.c_str());
    AppendMenuW(bottomPane, MF_STRING, ID_VIEW_BOTTOM_HEADINGS, ui.menuBottomHeadings.c_str());
    AppendMenuW(bottomPane, MF_STRING, ID_VIEW_BOTTOM_MATH, ui.menuBottomMath.c_str());
    AppendMenuW(bottomPane, MF_STRING, ID_VIEW_BOTTOM_ASSIST, ui.menuBottomAssist.c_str());
    AppendMenuW(view, MF_POPUP, reinterpret_cast<UINT_PTR>(bottomPane), ui.menuBottomPane.c_str());
    UINT noteWrapFlags = MF_STRING;
    if (g_noteWrapEnabled) noteWrapFlags |= MF_CHECKED;
    if (g_noteRenderEnabled && !g_noteRawOnly) noteWrapFlags |= MF_GRAYED;
    AppendMenuW(view, noteWrapFlags, ID_VIEW_NOTE_WRAP, text(L"menu.view.note_wrap").c_str());
    UINT leftPaneFlags = MF_STRING;
    if (!g_leftPaneCollapsed) leftPaneFlags |= MF_CHECKED;
    AppendMenuW(view, leftPaneFlags, ID_VIEW_LEFT_PANE_TOGGLE, text(L"menu.view.left_column").c_str());
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(view), ui.menuView.c_str());

    HMENU save = CreatePopupMenu();
    const bool hasStagedDiffs = file_output::HasAnyStagedDiffs();
    const UINT saveAllFlags = MenuStringState(CurrentNoteEditorIsModified() || g_noteDirty || g_annotsDirty ||
                                               g_noteNeedsIntegrate || g_annotsNeedsIntegrate || hasStagedDiffs ||
                                               (!g_currentSessionPath.empty() && g_currentNotePath.empty()));
    AppendMenuW(save, saveAllFlags, ID_FILE_SAVE_ALL, text(L"menu.save.work").c_str());
    AppendMenuW(save, hasStagedDiffs ? MF_STRING : (MF_STRING | MF_GRAYED), ID_OP_STAGE_MANAGE,
                text(L"menu.save.review_diffs").c_str());
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(save), ui.menuSave.c_str());

    HMENU restore = CreatePopupMenu();
    const UINT restorePdfFlags = menuState.hasPdfPositionBackup ? MF_STRING : (MF_STRING | MF_GRAYED);
    const UINT restoreLecFlags = menuState.hasLectureLastOpenBackup ? MF_STRING : (MF_STRING | MF_GRAYED);
    HMENU pdfPosition = CreatePopupMenu();
    AppendMenuW(pdfPosition, MF_STRING, ID_TEMP_RESET_PDF_POSITION, text(L"menu.common.delete").c_str());
    AppendMenuW(pdfPosition, restorePdfFlags, ID_TEMP_RESTORE_PDF_POSITION, text(L"menu.common.restore").c_str());
    AppendMenuW(restore, MF_POPUP, reinterpret_cast<UINT_PTR>(pdfPosition), text(L"menu.restore.pdf_position").c_str());
    HMENU lastOpen = CreatePopupMenu();
    AppendMenuW(lastOpen, MF_STRING, ID_TEMP_RESET_LECTURE_LAST_OPEN, text(L"menu.common.delete").c_str());
    AppendMenuW(lastOpen, restoreLecFlags, ID_TEMP_RESTORE_LECTURE_LAST_OPEN, text(L"menu.common.restore").c_str());
    AppendMenuW(restore, MF_POPUP, reinterpret_cast<UINT_PTR>(lastOpen), text(L"menu.restore.last_open").c_str());
    const UINT restoreFileHistoryFlags = menuState.hasSessionLastOpenBackup ? MF_STRING : (MF_STRING | MF_GRAYED);
    HMENU fileHistory = CreatePopupMenu();
    AppendMenuW(fileHistory, MF_STRING, ID_TEMP_RESET_SESSION_LAST_OPEN, text(L"menu.common.delete").c_str());
    AppendMenuW(fileHistory, restoreFileHistoryFlags, ID_TEMP_RESTORE_SESSION_LAST_OPEN,
                text(L"menu.common.restore").c_str());
    AppendMenuW(restore, MF_POPUP, reinterpret_cast<UINT_PTR>(fileHistory), text(L"menu.restore.file_history").c_str());
    HMENU backups = CreatePopupMenu();
    AppendMenuW(backups, MF_STRING, ID_FILE_RESTORE_BACKUP, text(L"menu.common.restore").c_str());
    AppendMenuW(backups, MF_STRING, ID_FILE_DELETE_BACKUP, text(L"menu.common.delete").c_str());
    AppendMenuW(restore, MF_POPUP, reinterpret_cast<UINT_PTR>(backups), text(L"menu.restore.backup").c_str());
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(restore), text(L"menu.bar.restore").c_str());

    HMENU exportMenu = CreatePopupMenu();
    const UINT exportPdfFlags = MenuStringState(CurrentLogicalPdfDocument() != nullptr);
    const UINT exportNoteFlags = MenuStringState(!g_currentNotePath.empty());
    AppendMenuW(exportMenu, exportPdfFlags, ID_FILE_EXPORT_PDF_QUICK, text(L"menu.export.quick_pdf").c_str());
    AppendMenuW(exportMenu, exportNoteFlags, ID_FILE_EXPORT_NOTE_TEXT_QUICK, text(L"menu.export.quick_note").c_str());
    const UINT exportDialogFlags = MenuStringState(CurrentLogicalPdfDocument() != nullptr || !g_currentNotePath.empty());
    AppendMenuW(exportMenu, exportDialogFlags, ID_FILE_EXPORT_COMBINED, text(L"menu.export.dialog").c_str());
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(exportMenu), ui.menuExport.c_str());

    HMENU tools = CreatePopupMenu();
    AppendMenuW(tools, MF_STRING, ID_GLOBAL_MEMOS, text(L"menu.tools.global_memos").c_str());
    HMENU viewer = CreatePopupMenu();
    AppendMenuW(viewer, MF_STRING, ID_OP_LAUNCH_READONLY_VIEWER, text(L"menu.viewer.launch").c_str());
    AppendMenuW(viewer, MF_STRING, ID_OP_OPEN_READONLY_VIEWER_FILE, text(L"menu.viewer.open_file").c_str());
    AppendMenuW(viewer, MF_STRING, ID_OP_CLOSE_ALL_READONLY_VIEWERS, text(L"menu.viewer.close_all").c_str());
    AppendMenuW(tools, MF_POPUP, reinterpret_cast<UINT_PTR>(viewer), text(L"menu.tools.viewer").c_str());
    const UINT readonlyViewerFlags = (!CurrentLogicalPdfPath().empty() &&
                                      IsPdfFile(std::filesystem::path(CurrentLogicalPdfPath())))
                                     ? MF_STRING : (MF_STRING | MF_GRAYED);
    AppendMenuW(tools, readonlyViewerFlags, ID_OP_OPEN_READONLY_VIEWER, text(L"menu.tools.open_in_viewer").c_str());
    if (!kIsLiteEdition) AppendMenuW(tools, MF_STRING, ID_OP_CONVERT_OFFICE_TO_PDF, ui.menuConvertOfficeToPdf.c_str());
    AppendMenuW(tools, MenuStringState(menuState.hasCurrentImage), ID_OP_CONVERT_IMAGE_TO_PDF,
                text(L"menu.tools.convert_image").c_str());
    AppendMenuW(tools, MenuStringState(!g_currentSessionPath.empty()), ID_OP_CREATE_BLANK_PDF, ui.menuCreateBlankPdf.c_str());
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(tools), text(L"menu.bar.tools").c_str());

    AppendMenuW(bar, MF_STRING, ID_SEARCH, ui.menuSearch.c_str());

    HMENU settings = CreatePopupMenu();
    AppendMenuW(settings, MF_STRING, ID_SETTINGS_GENERAL, text(L"menu.settings.dialog").c_str());
    AppendMenuW(settings, MF_STRING, ID_SETTINGS_PALETTE, text(L"menu.settings.palette").c_str());
    HMENU presets = CreatePopupMenu();
    AppendMenuW(presets, MF_STRING, ID_SETTINGS_PRESET_SAVE, text(L"menu.common.save").c_str());
    AppendMenuW(presets, MF_STRING, ID_SETTINGS_PRESET_LOAD, text(L"menu.common.load").c_str());
    AppendMenuW(settings, MF_POPUP, reinterpret_cast<UINT_PTR>(presets), text(L"menu.settings.presets").c_str());
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(settings), ui.menuSettings.c_str());

    HMENU help = CreatePopupMenu();
    AppendMenuW(help, MF_STRING, ID_HELP_GUIDE, text(L"menu.help.dialog").c_str());
    AppendMenuW(help, MF_STRING, ID_HELP_PDF_INFO, text(L"menu.help.pdf_info").c_str());
    AppendMenuW(help, MF_STRING, ID_HELP_NOTE_INFO, text(L"menu.help.note_info").c_str());
    AppendMenuW(help, MF_STRING, ID_HELP_SHOW_LOG_PATH, text(L"menu.help.log_path").c_str());
    if (menuState.developerMode) {
        HMENU debug = CreatePopupMenu();
        UINT debugToggleFlags = MF_STRING;
        if (menuState.debugLogsAllEnabled) debugToggleFlags |= MF_CHECKED;
        AppendMenuW(debug, debugToggleFlags, ID_DEBUG_LOG_TOGGLE_ALL,
                    DebugToggleLogsMenuLabel(menuState.debugLogsAllEnabled,
                                             menuState.debugLogsAnyEnabled).c_str());
        AppendMenuW(debug, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(debug, MF_STRING, ID_DEBUG_RESOURCE_MONITOR, DebugResourceMonitorMenuLabel().c_str());
        AppendMenuW(debug, MF_SEPARATOR, 0, nullptr);
        const UINT debugLogFlags = MenuStringState(menuState.hasWorkspaceLogFiles);
        AppendMenuW(debug, debugLogFlags, ID_DEBUG_LOG_ARCHIVE, DebugArchiveLogsMenuLabel().c_str());
        AppendMenuW(debug, debugLogFlags, ID_DEBUG_LOG_DELETE, DebugDeleteLogsMenuLabel().c_str());
        AppendMenuW(help, MF_POPUP, reinterpret_cast<UINT_PTR>(debug), DebugMenuLabel().c_str());
        AppendMenuW(help, MF_STRING, ID_HELP_CRASH, ui.menuCrash.c_str());
    }
    AppendMenuW(help, MF_STRING, ID_HELP_ABOUT, text(L"menu.help.about").c_str());
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(help), ui.menuHelp.c_str());

    const std::wstring initialStatusText = BuildStatusDisplayText();
    AppendMenuW(bar, MF_STRING | MF_DISABLED | MFT_RIGHTJUSTIFY, ID_STATUS_DISPLAY, initialStatusText.c_str());
    if (g_config.ownerDrawUi) ApplyMenuOwnerDraw(bar, true);
    UpdateBottomPaneMenuChecks();
    UpdateScrollDirectionMenuChecks();
    UpdatePdfSinglePageModeMenuCheck();
    return bar;
}

bool UpdateEditMenuUndoRedoState(HMENU menu) {
    if (!menu || GetMenuState(menu, ID_EDIT_UNDO, MF_BYCOMMAND) == static_cast<UINT>(-1)) {
        return false;
    }
    const bool workspaceUndoAllowed = !g_pdf.editingText;
    const bool canUndo = CanExecuteNoteUndoRedoFromFocus(true) || CanExecutePdfUndoRedoFromFocus(true) ||
                         (workspaceUndoAllowed && CanExecuteWorkspaceOperationUndoRedo(true));
    const bool canRedo = CanExecuteNoteUndoRedoFromFocus(false) || CanExecutePdfUndoRedoFromFocus(false) ||
                         (workspaceUndoAllowed && CanExecuteWorkspaceOperationUndoRedo(false));
    EnableMenuItem(menu, ID_EDIT_UNDO, MF_BYCOMMAND | (canUndo ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(menu, ID_EDIT_REDO, MF_BYCOMMAND | (canRedo ? MF_ENABLED : MF_GRAYED));
    return true;
}
