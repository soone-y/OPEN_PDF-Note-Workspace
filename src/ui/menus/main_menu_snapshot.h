#pragma once

struct MainMenuStateSnapshot {
    bool hasPdfPositionBackup = false;
    bool hasLectureLastOpenBackup = false;
    bool hasSessionLastOpenBackup = false;
    bool hasSavedFileBackup = false;
    bool hasCurrentPdf = false;
    bool hasCurrentNote = false;
    bool hasStagedDiffs = false;
    bool noteEditorDirty = false;
    bool noteDirty = false;
    bool annotsDirty = false;
    bool noteNeedsIntegrate = false;
    bool annotsNeedsIntegrate = false;
    bool hasCurrentNoteStage = false;
    bool noteWrapAvailable = false;
    bool noteWrapEnabled = false;
    bool exportPdfAvailable = false;
    bool exportNoteAvailable = false;
    bool hasWorkspaceLogFiles = false;
    bool debugLogsAllEnabled = false;
    bool debugLogsAnyEnabled = false;
    bool developerMode = false;
    bool leftPaneCollapsed = false;
    bool canUndo = false;
    bool canRedo = false;

    bool operator==(const MainMenuStateSnapshot& other) const {
        return hasPdfPositionBackup == other.hasPdfPositionBackup &&
               hasLectureLastOpenBackup == other.hasLectureLastOpenBackup &&
               hasSessionLastOpenBackup == other.hasSessionLastOpenBackup &&
               hasSavedFileBackup == other.hasSavedFileBackup &&
               hasCurrentPdf == other.hasCurrentPdf &&
               hasCurrentNote == other.hasCurrentNote &&
               hasStagedDiffs == other.hasStagedDiffs &&
               noteEditorDirty == other.noteEditorDirty &&
               noteDirty == other.noteDirty &&
               annotsDirty == other.annotsDirty &&
               noteNeedsIntegrate == other.noteNeedsIntegrate &&
               annotsNeedsIntegrate == other.annotsNeedsIntegrate &&
               hasCurrentNoteStage == other.hasCurrentNoteStage &&
               noteWrapAvailable == other.noteWrapAvailable &&
               noteWrapEnabled == other.noteWrapEnabled &&
               exportPdfAvailable == other.exportPdfAvailable &&
               exportNoteAvailable == other.exportNoteAvailable &&
               hasWorkspaceLogFiles == other.hasWorkspaceLogFiles &&
               debugLogsAllEnabled == other.debugLogsAllEnabled &&
               debugLogsAnyEnabled == other.debugLogsAnyEnabled &&
               developerMode == other.developerMode &&
               leftPaneCollapsed == other.leftPaneCollapsed &&
               canUndo == other.canUndo &&
               canRedo == other.canRedo;
    }
};

MainMenuStateSnapshot CaptureMainMenuStateSnapshot();
