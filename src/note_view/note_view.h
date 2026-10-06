// file: note_view/note_view.h
#pragma once
#include "core/app_core.h"
#include "core/text_encoding.h"
#include "bridge/view_bridge.h"
#include "note/note_identity.h"
#include "note/note_dirty_graph.h"
#include "note/note_render_final_ime_preedit_presentation.h"
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
    // Observation-only ownership facts for local UI automation.  They are
    // populated only from the immutable final frame and never drive paint,
    // input, persistence, or layout.
    bool finalFrameActive = false;
    // Last client paint, not a predicted decoration policy.
    bool currentLineHighlightPainted = false;
    bool finalCaretGeometryReady = false;
    RECT finalCaretClientRect{};
    // Allocated row envelope, including raw-height preservation padding.
    RECT finalCaretLineClientRect{};
    bool systemCaretMatchesFinal = false;
    size_t finalStructuredLineCount = 0;
    size_t finalRawLineCount = 0;
    bool documentReady = false;
    size_t mathSpanCount = 0;
    size_t diagnosticCount = 0;
    size_t renderedMathSegmentCount = 0;
    size_t renderedDisplayMathSegmentCount = 0;
    std::wstring renderedText;
    // Diagnostic-only canonical observation; excludes live IMM preedit text.
    std::wstring canonicalText;
    bool selectedMathFound = false;
    bool selectedMathIsBlock = false;
    std::wstring selectedMathDelimiter;
    std::wstring selectedMathNormalizedText;
};

// Scalar-only diagnostic observation. Unlike CaptureNoteUiSnapshot this
// never materializes text, hashes the note, or enumerates its rendered rows.
struct NoteRenderRuntimeObservation {
    bool finalFrameActive = false;
    bool structuralPublicationWasLocalPatch = false;
    uint64_t canonicalRevision = 0;
    uint64_t modelMaterializations = 0;
    unsigned nativeRedrawDepth = 0;
    bool nativeRedrawRequested = true;
    uint64_t nativeRedrawTransactions = 0;
    uint64_t clientPaintCount = 0;
    uint64_t nativeFallbackPaintCount = 0;
};
[[nodiscard]] NoteRenderRuntimeObservation ObserveNoteRenderRuntime() noexcept;

// Observation-only client rectangle, measured by the status assist's own
// layout. Returns false when its render control is not visible.
[[nodiscard]] bool GetNoteStatusAssistRenderControlRect(RECT* outRect);
enum class NoteStatusAssistControl {
    Rendering, Vim, NormalCaretRaw, ClickInsert, CurrentDisplay, InlineMathVerticalAlignment
};
[[nodiscard]] bool GetNoteStatusAssistControlRect(NoteStatusAssistControl control, RECT* outRect);

// Rebuild the immutable final frame after a persisted rendering preference
// changes outside the note view (for example, General settings).
void RefreshNoteRenderForPreferenceChange();

void RecomputeMathFromNote();
void LoadNoteFile(HWND hWnd, const std::wstring& path);
// Keep the rendered note visible after an open/focus command until a text
// editing input begins.
void PreserveRenderedNoteOpeningView();
// Emits focus, selection, and raw-line state to the opt-in preview trace.
// No note text or full path is included.
void TraceCurrentNoteFocusState(const wchar_t* origin);
bool SaveNoteFile(HWND hWnd);
void ClearCurrentNoteUndoHistory();
void ResetNoteEditHistorySnapshot(HWND hEdit);
void RecordCurrentNoteTextEditForUndo(HWND hEdit);
enum class NoteCanonicalTextChangeResult {
    Applied,
    Unchanged,
    Deferred,
    Unavailable,
};
// UI-thread acknowledgement of an observed native mutation. Unchanged is
// returned only after exact canonical/editor binding has been restored;
// notification presence or text length alone cannot prove it.
[[nodiscard]] NoteCanonicalTextChangeResult ObserveCurrentNoteTextChange(HWND hEdit) noexcept;
// Called for every RichEdit EN_CHANGE before the document kernel observes the
// mutation. It invalidates the zero-copy TextCore-to-editor binding until the
// canonical edit has been accepted.
void NoteEditorTextMutationObserved(HWND hEdit);
// Reconcile any RichEdit mutation with the document-local kernel before a
// command observes, saves, or replaces the current note.
[[nodiscard]] bool SynchronizeActiveNoteEditorToKernel(HWND owner);
// Undo/redo is document-local and may only change the focused editable note.
bool CanExecuteNoteUndoRedoFromFocus(bool undo);
bool ExecuteNoteUndoRedoFromFocus(HWND owner, bool undo);
void InsertSnippetIntoNote(const std::wstring& snippet);
bool InsertSnippetIntoCurrentNoteAt(size_t pos, const std::wstring& snippet);
void CommitPendingNoteClickCaret();
bool ShouldShowBottomNotePane();
// Input-driven updates must not block the RichEdit input path waiting for a
// bottom-pane paint.  State changes can still request an immediate repaint.
void RefreshBottomPaneView(bool synchronousPaint = true);
void UpdateNoteViewMode();
void EnsureInactiveCachedNoteEditWindowsParked();
void UpdateNoteLineSpacing(std::optional<note::NoteDirtyGraph> pendingGraph = std::nullopt);
void RefreshNoteLineSpacingForPresentationSurface(HWND hWnd);
void ExpandNoteRenderCanvasForPendingEdit(HWND hWnd);
void SyncNoteImeCandidateWindowToCaret(HWND hWnd, DWORD candidateMask = 0);
void ToggleNoteWrapSetting();
void SetNoteTyping(bool typing);
bool IsNoteTyping();
bool HasDeferredNoteFullReparse();
bool RequiresImmediateNoteDerivedFrameCommit();
void RunDeferredNoteFullReparseNow();
bool IsNoteChangeSuppressed();
bool IsNoteImeComposing();
// Non-polling input ownership. Native preedit EN_CHANGE must never publish
// provisional text into the canonical kernel, history, or stage requests.
[[nodiscard]] bool IsNoteImeCanonicalChangeDeferred(HWND hEdit);
// Called after one mixed result/preedit native dispatch. Proves the observed
// editor outcome against the captured canonical anchor before accepting only
// the committed result. Failure retains canonical data and native input.
[[nodiscard]] bool AcceptNoteImeResultWithContinuation(
    HWND hEdit, std::wstring_view result,
    const note::NoteImePreeditPayload& preedit) noexcept;
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
// Vim focus handoffs must use the same canonical/transport
// bridge as note input, rather than copying RichEdit positions into source.
[[nodiscard]] std::optional<std::pair<size_t, size_t>>
GetNoteEditorSelectionAsCanonical(HWND hWnd) noexcept;
[[nodiscard]] bool SetNoteEditorSelectionFromCanonical(HWND hWnd, size_t start, size_t end) noexcept;
bool HandleNoteNormalModeChar(HWND owner, wchar_t ch);
bool ClearNoteNormalVisualMode();
bool ClearNoteNormalPendingState();
bool IsNoteCmdlineActive();
bool IsNoteCmdlineImeComposing();
void SetNoteSearchResultMarker(size_t start, size_t end);
void ClearNoteSearchResultMarker();
NoteUiSnapshot CaptureNoteUiSnapshot();
note::SnapshotIdentity CaptureCurrentNoteSnapshotIdentity();
// Encode the active note for persistence using the encoding that was detected
// when this external text file was loaded.
bool CaptureCurrentNoteTextCoreForStorage(const std::wstring& expectedPath,
                                          std::string* outBytes,
                                          note::SnapshotIdentity* outIdentity,
                                          std::wstring* outError);
text_encoding::Encoding CurrentNoteStorageEncoding();
void FocusNoteEditForNormalMode();
// Observation-only text, shared with the three status-assist input rows.
[[nodiscard]] std::wstring CaptureNoteInputStatusText();
void CancelNoteCmdline();
LRESULT CALLBACK BottomNoteProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ノートのサブクラスウィンドウプロシージャ
LRESULT CALLBACK NoteEditProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
