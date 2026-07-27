// file: note_view/note_view.h
#pragma once
#include "core/app_core.h"
#include "bridge/view_bridge.h"
#include "file_output/file_output.h"
#include "note/note_identity.h"
#include "note/note_dirty_graph.h"
#include <optional>

// ノートテキストの読み書き・マークアップ解析・数式ビュー

struct NoteUiSnapshot {
    note::ViewIdentity viewIdentity{};
    note::SnapshotIdentity sourceIdentity{};
    bool changeSuppressed = false;
    bool programMutationActive = false;
    bool waitingForUserInput = false;
    size_t transactionDepth = 0;
    uint64_t transactionSequence = 0;
    note::NoteId transactionOwnerNoteId{};
    bool markdownRoute = false;
    bool renderOverlayActive = false;
    bool documentReady = false;
    size_t mathSpanCount = 0;
    size_t diagnosticCount = 0;
    size_t renderedMathSegmentCount = 0;
    size_t renderedDisplayMathSegmentCount = 0;
    std::wstring renderedText;
    bool selectedMathFound = false;
    bool selectedMathIsBlock = false;
    std::wstring selectedMathDelimiter;
    std::wstring selectedMathNormalizedText;
};

void RecomputeMathFromNote();
void LoadNoteFile(HWND hWnd, const std::wstring& path);
void ClearNoteEditorSilently(HWND hWnd, const std::wstring& nextNotePath = L"");
bool SaveNoteFile(HWND hWnd);
void ClearCurrentNoteUndoHistory();
void ResetNoteEditHistorySnapshot(HWND hEdit);
void RecordCurrentNoteTextEditForUndo(HWND hEdit);
// Undo/redo is document-local and may only change the focused editable note.
bool CanExecuteNoteUndoRedoFromFocus(bool undo);
bool ExecuteNoteUndoRedoFromFocus(HWND owner, bool undo);
void InsertSnippetIntoNote(const std::wstring& snippet);
bool InsertSnippetIntoCurrentNoteAt(size_t pos, const std::wstring& snippet);
void CommitPendingNoteClickCaret();
bool ShouldShowBottomNotePane();
void RefreshBottomPaneView();
void UpdateNoteViewMode();
void EnsureInactiveCachedNoteEditWindowsParked();
void UpdateNoteLineSpacing(std::optional<note::NoteDirtyGraph> pendingGraph = std::nullopt);
void ExpandNoteRenderCanvasForPendingEdit(HWND hWnd);
void SyncNoteImeCandidateWindowToCaret(HWND hWnd);
void ToggleNoteWrapSetting();
void SetNoteTyping(bool typing);
bool IsNoteTyping();
bool HasDeferredNoteFullReparse();
bool RequiresImmediateNoteDerivedFrameCommit();
void RunDeferredNoteFullReparseNow();
bool IsNoteChangeSuppressed();
bool IsNoteImeComposing();
bool CommitActiveNoteEditBoundary(HWND owner);
void ReleaseNoteChangeSuppressionForUserEdit();
// Render-mode switch support (avoid flicker during mode change).
bool BeginNoteRenderSwitch();
bool IsNoteRenderSwitchInProgress();
bool CommitNoteRenderStateBeforePaintIfNeeded(HWND hWnd);
void EndNoteRenderSwitch();
bool CommitNoteImeCompositionNow();
void OnEnterNoteNormalMode();
void OnExitNoteNormalMode();
bool HandleNoteNormalModeChar(HWND owner, wchar_t ch);
bool ClearNoteNormalVisualMode();
bool ClearNoteNormalPendingState();
bool IsNoteCmdlineActive();
bool IsNoteCmdlineImeComposing();
void SetNoteSearchResultMarker(size_t start, size_t end);
void ClearNoteSearchResultMarker();
NoteUiSnapshot CaptureNoteUiSnapshot();
note::SnapshotIdentity CaptureCurrentNoteSnapshotIdentity();
void FocusMainWindowForNoteNormalMode();
void CancelNoteCmdline();
LRESULT CALLBACK BottomNoteProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ノートのサブクラスウィンドウプロシージャ
LRESULT CALLBACK NoteEditProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
