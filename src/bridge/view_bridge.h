// file: bridge/view_bridge.h
// Cross-view APIs (pdf_view <-> note_view). Implementations stay in each view TU.
#pragma once

#include "core/app_core.h"
#include "core/text_encoding.h"
#include "note/note_identity.h"

#include <cstdint>
#include <optional>
#include <string>

// Persistence may need a view-local restore point, but must not depend on a
// concrete editor control or on note_view implementation details.
struct NoteEditorPersistenceState {
    std::uint64_t contentRevision = 0;
    std::uint64_t selectionStart = 0;
    std::uint64_t selectionEnd = 0;
    int scrollX = 0;
    int scrollY = 0;
    int firstVisibleLine = 0;
};

void JumpToPage(HWND pdfWnd, int index);
void JumpToPdfPoint(HWND pdfWnd, int pageIndex, double xPt, double yPt);
bool OpenPdfWithAnnotations(HWND owner, const std::wstring& path);
bool AddMathAnnotationFromTextAtPoint(HWND pdfWnd, const std::wstring& rawText, MathKind kind, const POINT& screenPt);
bool AddMathAnnotationFromText(HWND pdfWnd, const std::wstring& rawText, MathKind kind);

void LoadNoteFile(HWND hWnd, const std::wstring& path);
void ClearNoteEditorSilently(HWND hWnd, const std::wstring& nextNotePath = L"");
void RefreshCurrentNoteBottomPane(bool synchronousPaint = true);
[[nodiscard]] bool ShouldShowBottomNotePane();
void EnsureInactiveCachedNoteEditWindowsParked();
void ExitCurrentNoteNormalMode();
void RefreshCurrentNoteAfterFontChange();
void ApplyCurrentNoteViewConfiguration();
void ClearCurrentNoteSearchMarker();
void SetCurrentNoteSearchMarker(size_t start, size_t end);
[[nodiscard]] bool SelectCurrentNoteLine(int oneBasedLine);
void RefreshCurrentNoteFileSnapshot();
void RefreshCurrentNotePersistenceIdentity(const std::wstring& path);
bool CaptureCurrentNoteTextCoreUtf8(const std::wstring& expectedPath,
                                    std::string* outBytes,
                                    note::SnapshotIdentity* outIdentity = nullptr);
bool CaptureCurrentNoteTextCoreForStorage(const std::wstring& expectedPath,
                                          std::string* outBytes,
                                          note::SnapshotIdentity* outIdentity = nullptr,
                                          std::wstring* outError = nullptr);
text_encoding::Encoding CurrentNoteStorageEncoding();
note::SnapshotIdentity CaptureCurrentNoteSnapshotIdentity();
bool HasCurrentNoteEditorForPath(const std::wstring& expectedPath);
bool CurrentNoteEditorIsModified();
int CurrentNoteEditorCharacterCount();
std::wstring ReadCurrentNoteEditorText();
std::optional<NoteEditorPersistenceState> CaptureCurrentNoteEditorPersistenceState(
    const std::wstring& expectedPath, std::uint64_t contentRevision);
void MarkCurrentNoteEditorPersisted(const std::wstring& expectedPath);
void ClearCurrentNoteUndoHistory();
[[nodiscard]] bool SynchronizeActiveNoteEditorToKernel(HWND owner);
[[nodiscard]] bool CommitActiveNoteEditBoundary(HWND owner);
bool CanExecuteNoteUndoRedoFromFocus(bool undo);
bool CheckCurrentNoteFileExternalChange(HWND owner);
bool JumpToNoteLinkId(const std::wstring& linkId,
                      const std::wstring& notePath,
                      bool showNotFoundNotice = true,
                      const std::wstring& sourceNotePath = L"",
                      std::optional<size_t> sourcePos = std::nullopt);

bool IsNoteTyping();
bool IsNoteImeComposing();
