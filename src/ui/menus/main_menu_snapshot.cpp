#include "ui/menus/main_menu_snapshot.h"

#include "core/app_core.h"
#include "core/path_safety.h"
#include "core/preview_trace.h"
#include "file_output/file_output.h"
#include "app/main_escape_backup.h"
#include "workspace/main_workspace_logs.h"
#include "pdf_view/pdf_view.h"
#include "note_view/note_view.h"

#include <windows.h>

namespace {

bool AreAllDebugLogsEnabled(const AppDebugLogConfig& cfg) {
    return cfg.previewTrace && cfg.switchTiming && cfg.crash && cfg.startupWatchdog;
}

bool AreAnyDebugLogsEnabled(const AppDebugLogConfig& cfg) {
    return cfg.previewTrace || cfg.switchTiming || cfg.crash || cfg.startupWatchdog;
}

bool HasExportablePdf() {
    return CurrentLogicalPdfDocument() != nullptr;
}

bool HasExportableNote() {
    return !g_currentNotePath.empty();
}

} // namespace

MainMenuStateSnapshot CaptureMainMenuStateSnapshot() {
    const ULONGLONG startTick = preview_trace::TickNow();
    MainMenuStateSnapshot state;
    const EscapeBackupPresence backupPresence = ScanEscapeBackupPresence();
    state.hasPdfPositionBackup = backupPresence.hasPdfPositionBackup;
    state.hasLectureLastOpenBackup = backupPresence.hasLectureLastOpenBackup;
    state.hasSessionLastOpenBackup = backupPresence.hasSessionLastOpenBackup;
    state.hasSavedFileBackup = backupPresence.hasSavedFileBackup;
    preview_trace::Append(
        L"CaptureMainMenuStateSnapshot",
        L"after_backup_checks elapsed_ms=" + preview_trace::ElapsedMs(startTick) +
        L" pdfBackup=" + preview_trace::Bool(state.hasPdfPositionBackup) +
        L" lectureBackup=" + preview_trace::Bool(state.hasLectureLastOpenBackup) +
        L" sessionBackup=" + preview_trace::Bool(state.hasSessionLastOpenBackup) +
        L" savedFileBackup=" + preview_trace::Bool(state.hasSavedFileBackup));
    state.hasCurrentPdf = HasExportablePdf();
    state.hasCurrentImage = g_pdf.kind == DocKind::Image;
    state.hasCurrentNote = !g_currentNotePath.empty();
    state.hasStagedDiffs = file_output::HasAnyStagedDiffs();
    preview_trace::Append(
        L"CaptureMainMenuStateSnapshot",
        L"after_has_staged elapsed_ms=" + preview_trace::ElapsedMs(startTick) +
        L" hasStagedDiffs=" + preview_trace::Bool(state.hasStagedDiffs));
    state.noteEditorDirty = g_hNoteEdit && (SendMessageW(g_hNoteEdit, EM_GETMODIFY, 0, 0) != 0);
    state.noteDirty = g_noteDirty;
    state.annotsDirty = g_annotsDirty;
    state.noteNeedsIntegrate = g_noteNeedsIntegrate;
    state.annotsNeedsIntegrate = g_annotsNeedsIntegrate;
    state.hasCurrentNoteStage = g_noteNeedsIntegrate;
    if (!state.hasCurrentNoteStage && !g_currentNotePath.empty()) {
        const std::wstring currentNoteKey = NormalizePathKey(g_currentNotePath);
        for (const auto& entry : file_output::ListStagedDiffEntries()) {
            if (entry.kind != file_output::StagedDiffKind::Note) continue;
            if (NormalizePathKey(entry.targetPath) == currentNoteKey) {
                state.hasCurrentNoteStage = true;
                break;
            }
        }
    }
    state.noteWrapAvailable = !(g_noteRenderEnabled && !g_noteRawOnly);
    state.noteWrapEnabled = g_noteWrapEnabled;
    state.exportPdfAvailable = HasExportablePdf();
    state.exportNoteAvailable = HasExportableNote();
    state.hasWorkspaceLogFiles = HasWorkspaceLogFiles();
    state.debugLogsAllEnabled = AreAllDebugLogsEnabled(g_config.debugLogs);
    state.debugLogsAnyEnabled = AreAnyDebugLogsEnabled(g_config.debugLogs);
    state.developerMode = g_config.developerMode;
    state.leftPaneCollapsed = g_leftPaneCollapsed;
    // The focused editor is the only undo/redo target. Do not fall back to a
    // previously active document when a list, dialog, or no control is focused.
    state.canUndo = CanExecuteNoteUndoRedoFromFocus(true) || CanExecutePdfUndoRedoFromFocus(true);
    state.canRedo = CanExecuteNoteUndoRedoFromFocus(false) || CanExecutePdfUndoRedoFromFocus(false);
    preview_trace::Append(
        L"CaptureMainMenuStateSnapshot",
        L"end elapsed_ms=" + preview_trace::ElapsedMs(startTick) +
        L" hasCurrentPdf=" + preview_trace::Bool(state.hasCurrentPdf) +
        L" hasCurrentNote=" + preview_trace::Bool(state.hasCurrentNote));
    return state;
}
