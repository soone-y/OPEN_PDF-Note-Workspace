#include "ui/menus/menu_build.h"

#include "core/app_core.h"
#include "core/localization.h"
#include "core/path_safety.h"
#include "file_output/file_output.h"
#include "ui/menus/main_debug_menu.h"
#include "ui/menus/main_menu_owner_draw.h"
#include "ui/menus/main_status_display.h"
#include "pdf_view/pdf_view.h"
#include "note_view/note_view.h"
#include "settings/settings.h"

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

    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, ID_FILE_NEW_LECTURE, ui.menuNewLecture.c_str());
    AppendMenuW(file, MF_STRING, ID_FILE_NEW_CLRO, ui.menuNewClro.c_str());
    AppendMenuW(file, MF_STRING, ID_FILE_IMPORT_FILE, ui.menuImportFile.c_str());
    AppendMenuW(file, MF_STRING, ID_FILE_IMPORT_DIR_AS_LECTURE, ui.menuImportDirAsLecture.c_str());
    AppendMenuW(file, MenuStringState(!g_currentLecturePath.empty()),
                ID_FILE_IMPORT_DIR_AS_SESSION, ui.menuImportDirAsSession.c_str());
    AppendMenuW(file, MF_STRING, ID_FILE_ORGANIZE_SESSION_FILES, ui.menuOrganizeSessionFiles.c_str());
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, ID_FILE_OPEN_WS, ui.menuOpenWs.c_str());
    AppendMenuW(file, MF_STRING, ID_FILE_RELOAD_WS, ui.menuReloadWs.c_str());
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);

    HMENU temp = CreatePopupMenu();
    AppendMenuW(temp, MF_STRING, ID_FILE_ADD_TEMP_EXTERNAL_LECTURE, ui.menuAddTempExternalLecture.c_str());
    AppendMenuW(temp, MF_STRING, ID_FILE_REMOVE_TEMP_EXTERNAL_LECTURE, ui.menuRemoveTempExternalLecture.c_str());
    AppendMenuW(temp, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(temp, MF_STRING, ID_TEMP_RESET_PDF_POSITION, ui.menuResetPdfPosition.c_str());
    AppendMenuW(temp, MF_STRING, ID_TEMP_RESET_LECTURE_LAST_OPEN, ui.menuResetLectureLastOpen.c_str());
    AppendMenuW(temp, MF_STRING, ID_TEMP_RESET_SESSION_LAST_OPEN, ui.menuResetSessionLastOpen.c_str());
    AppendMenuW(temp, MF_SEPARATOR, 0, nullptr);
    HMENU tempOps = CreatePopupMenu();
    HMENU tempRestore = CreatePopupMenu();
    HMENU tempDelete = CreatePopupMenu();
    const UINT restorePdfFlags = menuState.hasPdfPositionBackup ? MF_STRING : (MF_STRING | MF_GRAYED);
    const UINT restoreLecFlags = menuState.hasLectureLastOpenBackup ? MF_STRING : (MF_STRING | MF_GRAYED);
    const UINT restoreSesFlags = menuState.hasSessionLastOpenBackup ? MF_STRING : (MF_STRING | MF_GRAYED);
    AppendMenuW(tempRestore, restorePdfFlags, ID_TEMP_RESTORE_PDF_POSITION, ui.menuRestorePdfPosition.c_str());
    AppendMenuW(tempRestore, restoreLecFlags, ID_TEMP_RESTORE_LECTURE_LAST_OPEN, ui.menuRestoreLectureLastOpen.c_str());
    AppendMenuW(tempRestore, restoreSesFlags, ID_TEMP_RESTORE_SESSION_LAST_OPEN, ui.menuRestoreSessionLastOpen.c_str());
    AppendMenuW(tempDelete, restorePdfFlags, ID_TEMP_DELETE_PDF_POSITION_BACKUP, ui.menuDeletePdfPositionBackup.c_str());
    AppendMenuW(tempDelete, restoreLecFlags, ID_TEMP_DELETE_LECTURE_LAST_OPEN_BACKUP, ui.menuDeleteLectureLastOpenBackup.c_str());
    AppendMenuW(tempDelete, restoreSesFlags, ID_TEMP_DELETE_SESSION_LAST_OPEN_BACKUP, ui.menuDeleteSessionLastOpenBackup.c_str());
    AppendMenuW(tempOps, MF_POPUP, reinterpret_cast<UINT_PTR>(tempRestore), ui.menuTempRestore.c_str());
    AppendMenuW(tempOps, MF_POPUP, reinterpret_cast<UINT_PTR>(tempDelete), ui.menuTempDelete.c_str());
    AppendMenuW(temp, MF_POPUP, reinterpret_cast<UINT_PTR>(tempOps), ui.menuTempOperation.c_str());
    AppendMenuW(file, MF_POPUP, reinterpret_cast<UINT_PTR>(temp), ui.menuTemp.c_str());

    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, ID_FILE_OPEN_WORKSPACE_DIR, ui.menuOpenWorkspaceDir.c_str());
    AppendMenuW(file, MF_STRING, ID_FILE_OPEN_LECTURE_DIR, ui.menuOpenLectureDir.c_str());
    // Keep this available like "Open Lecture Folder"; the command reports a
    // quiet visual warning when no session is selected.
    AppendMenuW(file, MF_STRING, ID_FILE_OPEN_SESSION_DIR, ui.menuOpenSessionDir.c_str());
    AppendMenuW(file, MF_STRING, ID_FILE_EXIT, ui.menuExit.c_str());
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), ui.menuFile.c_str());

    HMENU edit = CreatePopupMenu();
    AppendMenuW(edit, MenuStringState(menuState.canUndo), ID_EDIT_UNDO,
                localization::Text(L"menu.edit.undo").c_str());
    AppendMenuW(edit, MenuStringState(menuState.canRedo), ID_EDIT_REDO,
                localization::Text(L"menu.edit.redo").c_str());
    const std::wstring editLabel = localization::Text(L"menu.edit.title");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(edit), editLabel.c_str());

    HMENU op = CreatePopupMenu();
    UINT renamePdfFlags = !CurrentLogicalPdfPath().empty() ? MF_STRING : (MF_STRING | MF_GRAYED);
    UINT renameNoteFlags = !g_currentNotePath.empty() ? MF_STRING : (MF_STRING | MF_GRAYED);
    UINT readonlyViewerFlags = (!CurrentLogicalPdfPath().empty() &&
                                IsPdfFile(std::filesystem::path(CurrentLogicalPdfPath())))
                                   ? MF_STRING
                                   : (MF_STRING | MF_GRAYED);
    AppendMenuW(op, MF_STRING, ID_OP_LAUNCH_READONLY_VIEWER, ui.menuLaunchReadOnlyViewer.c_str());
    AppendMenuW(op, readonlyViewerFlags, ID_OP_OPEN_READONLY_VIEWER, ui.menuOpenReadOnlyViewer.c_str());
    AppendMenuW(op, MF_STRING, ID_OP_CLOSE_ALL_READONLY_VIEWERS, ui.menuCloseAllReadOnlyViewers.c_str());
    AppendMenuW(op, MF_SEPARATOR, 0, nullptr);
    if (!kIsLiteEdition) {
        AppendMenuW(op, MF_STRING, ID_OP_CONVERT_OFFICE_TO_PDF, ui.menuConvertOfficeToPdf.c_str());
    }
    AppendMenuW(op, MenuStringState(menuState.hasCurrentImage), ID_OP_CONVERT_IMAGE_TO_PDF,
                localization::Text(L"menu.operation.convert_image_to_pdf").c_str());
    AppendMenuW(op, MenuStringState(!g_currentSessionPath.empty()),
                ID_OP_CREATE_BLANK_PDF, ui.menuCreateBlankPdf.c_str());
    AppendMenuW(op, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(op, renamePdfFlags, ID_OP_RENAME_PDF, ui.menuRenamePdf.c_str());
    AppendMenuW(op, renameNoteFlags, ID_OP_RENAME_NOTE, ui.menuRenameNote.c_str());
    AppendMenuW(op, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(op, renamePdfFlags, ID_OP_MOVE_PDF, ui.menuMovePdf.c_str());
    AppendMenuW(op, renameNoteFlags, ID_OP_MOVE_NOTE, ui.menuMoveNote.c_str());
    AppendMenuW(op, MF_SEPARATOR, 0, nullptr);
    UINT diffManagerFlags = file_output::HasAnyStagedDiffs() ? MF_STRING : (MF_STRING | MF_GRAYED);
    AppendMenuW(op, diffManagerFlags, ID_OP_STAGE_MANAGE, ui.menuDiffManager.c_str());
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(op), ui.menuOperation.c_str());

    HMENU view = CreatePopupMenu();
    const UINT pdfViewFlags = MenuStringState(menuState.hasCurrentPdf);
    AppendMenuW(view, pdfViewFlags, ID_VIEW_RESET_ZOOM, ui.menuResetZoom.c_str());
    AppendMenuW(view, pdfViewFlags, ID_VIEW_SET_ZOOM, ui.menuSetZoom.c_str());
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    std::wstring leftPaneLabel = localization::Text(L"menu.view.left_column");
    UINT leftPaneFlags = MF_STRING;
    if (!g_leftPaneCollapsed) leftPaneFlags |= MF_CHECKED;
    AppendMenuW(view, leftPaneFlags, ID_VIEW_LEFT_PANE_TOGGLE, leftPaneLabel.c_str());
    UINT readableTextFlags = MF_STRING;
    if (g_readableTextOverlay) readableTextFlags |= MF_CHECKED;
    AppendMenuW(view, readableTextFlags, ID_VIEW_READABLE_TEXT_OVERLAY,
                localization::Text(L"menu.view.readable_low_contrast").c_str());
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);

    HMENU scrollDir = CreatePopupMenu();
    g_hScrollDirectionMenu = scrollDir;
    AppendMenuW(scrollDir, MF_STRING, ID_VIEW_SCROLL_DIR_V_TTB, ui.menuScrollDirVTopToBottom.c_str());
    AppendMenuW(scrollDir, MF_STRING, ID_VIEW_SCROLL_DIR_V_BTU, ui.menuScrollDirVBottomToTop.c_str());
    AppendMenuW(scrollDir, MF_STRING, ID_VIEW_SCROLL_DIR_H_RTL, ui.menuScrollDirHRightToLeft.c_str());
    AppendMenuW(scrollDir, MF_STRING, ID_VIEW_SCROLL_DIR_H_LTR, ui.menuScrollDirHLeftToRight.c_str());
    AppendMenuW(view, MF_POPUP, reinterpret_cast<UINT_PTR>(scrollDir), ui.menuScrollDirection.c_str());
    UINT singlePageFlags = MF_STRING;
    if (g_config.pdfSinglePageMode) singlePageFlags |= MF_CHECKED;
    AppendMenuW(view, singlePageFlags, ID_VIEW_PDF_SINGLE_PAGE_MODE,
                ui.menuPdfSinglePageMode.c_str());

    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view, pdfViewFlags, ID_VIEW_FIRST_PAGE, ui.menuFirstPage.c_str());
    AppendMenuW(view, pdfViewFlags, ID_VIEW_PREV_PAGE, ui.menuPrevPage.c_str());
    AppendMenuW(view, pdfViewFlags, ID_VIEW_NEXT_PAGE, ui.menuNextPage.c_str());
    AppendMenuW(view, pdfViewFlags, ID_VIEW_LAST_PAGE, ui.menuLastPage.c_str());
    AppendMenuW(view, pdfViewFlags, ID_VIEW_JUMP_PAGE, ui.menuJumpPage.c_str());
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    HMENU bottomPane = CreatePopupMenu();
    g_hBottomPaneMenu = bottomPane;
    AppendMenuW(bottomPane, MF_STRING, ID_VIEW_BOTTOM_NOTE, ui.menuBottomNote.c_str());
    AppendMenuW(bottomPane, MF_STRING, ID_VIEW_BOTTOM_HEADINGS, ui.menuBottomHeadings.c_str());
    AppendMenuW(bottomPane, MF_STRING, ID_VIEW_BOTTOM_MATH, ui.menuBottomMath.c_str());
    AppendMenuW(bottomPane, MF_STRING, ID_VIEW_BOTTOM_ASSIST, ui.menuBottomAssist.c_str());
    AppendMenuW(view, MF_POPUP, reinterpret_cast<UINT_PTR>(bottomPane), ui.menuBottomPane.c_str());
    const bool noteWrapAvailable = !(g_noteRenderEnabled && !g_noteRawOnly);
    UINT noteWrapFlags = MF_STRING;
    if (g_noteWrapEnabled) noteWrapFlags |= MF_CHECKED;
    if (!noteWrapAvailable) noteWrapFlags |= MF_GRAYED;
    AppendMenuW(view, noteWrapFlags, ID_VIEW_NOTE_WRAP, ui.menuNoteWrap.c_str());
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(view), ui.menuView.c_str());

    HMENU save = CreatePopupMenu();
    HMENU exportMenu = CreatePopupMenu();
    const UINT exportPdfFlags = MenuStringState(CurrentLogicalPdfDocument() != nullptr);
    const UINT exportNoteFlags = MenuStringState(!g_currentNotePath.empty());
    const bool noteEditorDirty = g_hNoteEdit && (SendMessageW(g_hNoteEdit, EM_GETMODIFY, 0, 0) != 0);
    const bool hasStagedDiffs = file_output::HasAnyStagedDiffs();
    const bool canCreateDeferredNote = !g_currentSessionPath.empty() && g_currentNotePath.empty();
    bool hasCurrentNoteStage = g_noteNeedsIntegrate;
    if (!hasCurrentNoteStage && !g_currentNotePath.empty()) {
        const std::wstring currentNoteKey = NormalizePathKey(g_currentNotePath);
        for (const auto& entry : file_output::ListStagedDiffEntries()) {
            if (entry.kind != file_output::StagedDiffKind::Note) continue;
            if (NormalizePathKey(entry.targetPath) == currentNoteKey) {
                hasCurrentNoteStage = true;
                break;
            }
        }
    }
    const UINT saveAllFlags = MenuStringState(noteEditorDirty || g_noteDirty || g_annotsDirty ||
                                               g_noteNeedsIntegrate || g_annotsNeedsIntegrate ||
                                               hasStagedDiffs || canCreateDeferredNote);
    const UINT saveNoteFlags = MenuStringState(!g_currentNotePath.empty() &&
                                               (noteEditorDirty || g_noteDirty || hasCurrentNoteStage));
    const UINT saveDiffManagerFlags = hasStagedDiffs ? MF_STRING : (MF_STRING | MF_GRAYED);
    const UINT deleteBackupFlags = menuState.hasSavedFileBackup ? MF_STRING : (MF_STRING | MF_GRAYED);
    AppendMenuW(save, saveAllFlags, ID_FILE_SAVE_ALL, ui.menuSaveAll.c_str());
    AppendMenuW(save, saveNoteFlags, ID_FILE_SAVE_NOTE, ui.menuSaveNote.c_str());
    AppendMenuW(save, saveDiffManagerFlags, ID_OP_STAGE_MANAGE, ui.menuDiffManager.c_str());
    AppendMenuW(save, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(save, MF_STRING, ID_FILE_RECOVERY, ui.menuRecovery.c_str());
    AppendMenuW(save, MF_STRING, ID_FILE_RESTORE_BACKUP, localization::Text(L"menu.save.restore_backup").c_str());
    AppendMenuW(save, deleteBackupFlags, ID_FILE_DELETE_BACKUP, ui.menuDeleteBackup.c_str());
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(save), ui.menuSave.c_str());

    const UINT exportCombinedFlags = MenuStringState(g_pdf.doc != nullptr || !g_currentNotePath.empty());
    AppendMenuW(exportMenu, exportPdfFlags, ID_FILE_EXPORT_PDF_QUICK,
                localization::Text(L"menu.export.quick_pdf").c_str());
    AppendMenuW(exportMenu, exportNoteFlags, ID_FILE_EXPORT_NOTE_TEXT_QUICK,
                localization::Text(L"menu.export.quick_note_text").c_str());
    AppendMenuW(exportMenu, exportNoteFlags, ID_FILE_EXPORT_NOTE_MARKDOWN_QUICK,
                localization::Text(L"menu.export.quick_note_markdown").c_str());
    AppendMenuW(exportMenu, exportNoteFlags, ID_FILE_EXPORT_NOTE_HTML_QUICK,
                localization::Text(L"menu.export.quick_note_html").c_str());
    AppendMenuW(exportMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(exportMenu, exportCombinedFlags, ID_FILE_EXPORT_COMBINED,
                localization::Text(L"menu.export.combined").c_str());
    AppendMenuW(exportMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(exportMenu, exportPdfFlags, ID_FILE_EXPORT_PDF,
                localization::Text(L"menu.export.open_pdf").c_str());
    AppendMenuW(exportMenu, exportPdfFlags, ID_FILE_EXPORT_PDF_PAGES, ui.menuExportPdfPages.c_str());
    AppendMenuW(exportMenu, exportPdfFlags, ID_FILE_EXPORT_PNG_PAGE, ui.menuExportPngPage.c_str());
    AppendMenuW(exportMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(exportMenu, exportNoteFlags, ID_FILE_EXPORT_NOTE_TEXT,
                localization::Text(L"menu.export.open_note").c_str());
    AppendMenuW(exportMenu, exportNoteFlags, ID_FILE_EXPORT_NOTE_MARKUP, ui.menuExportNoteMarkup.c_str());
    AppendMenuW(exportMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(exportMenu, MF_STRING, ID_SETTINGS_BUNDLE_EXPORT,
                localization::Text(L"menu.settings.export_migration").c_str());
    AppendMenuW(exportMenu, MF_STRING, ID_SETTINGS_BUNDLE_IMPORT,
                localization::Text(L"menu.settings.import_migration").c_str());
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(exportMenu), ui.menuExport.c_str());

    AppendMenuW(bar, MF_STRING, ID_SEARCH, ui.menuSearch.c_str());

    HMENU help = CreatePopupMenu();
    AppendMenuW(help, MF_STRING, ID_HELP_GUIDE, ui.menuHelpGuide.c_str());
    AppendMenuW(help, MF_STRING, ID_HELP_PDF_INFO, ui.menuPdfInfo.c_str());
    AppendMenuW(help, MF_STRING, ID_HELP_SHOW_LOG_PATH, localization::Text(L"menu.help.show_log_path").c_str());
    if (menuState.developerMode) {
        AppendMenuW(help, MF_SEPARATOR, 0, nullptr);
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
        AppendMenuW(help, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(help, MF_STRING, ID_HELP_CRASH, ui.menuCrash.c_str());
    }
    AppendMenuW(help, MF_STRING, ID_HELP_ABOUT, ui.menuAbout.c_str());
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(help), ui.menuHelp.c_str());

    HMENU settings = CreatePopupMenu();
    HMENU bottomPaneSettings = CreatePopupMenu();
    g_hBottomPaneMenuSettings = bottomPaneSettings;
    AppendMenuW(bottomPaneSettings, MF_STRING, ID_VIEW_BOTTOM_NOTE, ui.menuBottomNote.c_str());
    AppendMenuW(bottomPaneSettings, MF_STRING, ID_VIEW_BOTTOM_HEADINGS, ui.menuBottomHeadings.c_str());
    AppendMenuW(bottomPaneSettings, MF_STRING, ID_VIEW_BOTTOM_MATH, ui.menuBottomMath.c_str());
    AppendMenuW(bottomPaneSettings, MF_STRING, ID_VIEW_BOTTOM_ASSIST, ui.menuBottomAssist.c_str());
    AppendMenuW(settings, MF_POPUP, reinterpret_cast<UINT_PTR>(bottomPaneSettings), ui.menuBottomPane.c_str());
    AppendMenuW(settings, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(settings, MF_STRING, ID_SETTINGS_GENERAL, ui.menuGeneralSettings.c_str());
    AppendMenuW(settings, MF_STRING, ID_SETTINGS_NOTE, ui.menuNoteSettings.c_str());
    AppendMenuW(settings, MF_STRING, ID_SETTINGS_ANNOTATION, ui.menuAnnotSettings.c_str());
    AppendMenuW(settings, MF_STRING, ID_SETTINGS_MARKUP, ui.menuMarkupSettings.c_str());
    AppendMenuW(settings, MF_STRING, ID_SETTINGS_PALETTE, ui.menuPaletteSettings.c_str());
    AppendMenuW(settings, MF_STRING, ID_GLOBAL_MEMOS,
                localization::Text(L"menu.settings.global_memos").c_str());
    AppendMenuW(settings, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(settings, MF_STRING, ID_SETTINGS_PRESET_SAVE, ui.menuSettingsPresetSave.c_str());
    AppendMenuW(settings, MF_STRING, ID_SETTINGS_PRESET_LOAD, ui.menuSettingsPresetLoad.c_str());
    AppendMenuW(settings, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(settings, MF_STRING, ID_SETTINGS_BUNDLE_EXPORT,
                localization::Text(L"menu.settings.export_migration").c_str());
    AppendMenuW(settings, MF_STRING, ID_SETTINGS_BUNDLE_IMPORT,
                localization::Text(L"menu.settings.import_migration").c_str());
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(settings), ui.menuSettings.c_str());

    const std::wstring initialStatusText = BuildStatusDisplayText();
    AppendMenuW(bar, MF_STRING | MF_DISABLED | MFT_RIGHTJUSTIFY, ID_STATUS_DISPLAY, initialStatusText.c_str());

    if (g_config.ownerDrawUi) {
        ApplyMenuOwnerDraw(bar, true);
    }
    UpdateBottomPaneMenuChecks();
    UpdateScrollDirectionMenuChecks();
    UpdatePdfSinglePageModeMenuCheck();
    return bar;
}

bool UpdateEditMenuUndoRedoState(HMENU menu) {
    if (!menu || GetMenuState(menu, ID_EDIT_UNDO, MF_BYCOMMAND) == static_cast<UINT>(-1)) {
        return false;
    }
    const bool canUndo = CanExecuteNoteUndoRedoFromFocus(true) || CanExecutePdfUndoRedoFromFocus(true);
    const bool canRedo = CanExecuteNoteUndoRedoFromFocus(false) || CanExecutePdfUndoRedoFromFocus(false);
    EnableMenuItem(menu, ID_EDIT_UNDO, MF_BYCOMMAND | (canUndo ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(menu, ID_EDIT_REDO, MF_BYCOMMAND | (canRedo ? MF_ENABLED : MF_GRAYED));
    return true;
}
