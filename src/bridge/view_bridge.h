// file: bridge/view_bridge.h
// Cross-view APIs (pdf_view <-> note_view). Implementations stay in each view TU.
#pragma once

#include "core/app_core.h"
#include "note/note_identity.h"

#include <optional>
#include <string>

void JumpToPage(HWND pdfWnd, int index);
void JumpToPdfPoint(HWND pdfWnd, int pageIndex, double xPt, double yPt);
bool OpenPdfWithAnnotations(HWND owner, const std::wstring& path);
bool AddMathAnnotationFromTextAtPoint(HWND pdfWnd, const std::wstring& rawText, MathKind kind, const POINT& screenPt);
bool AddMathAnnotationFromText(HWND pdfWnd, const std::wstring& rawText, MathKind kind);

void LoadNoteFile(HWND hWnd, const std::wstring& path);
void RefreshCurrentNoteFileSnapshot();
void RefreshCurrentNotePersistenceIdentity(const std::wstring& path);
bool CaptureCurrentNoteTextCoreUtf8(const std::wstring& expectedPath,
                                    std::string* outBytes,
                                    note::SnapshotIdentity* outIdentity = nullptr);
note::SnapshotIdentity CaptureCurrentNoteSnapshotIdentity();
bool CheckCurrentNoteFileExternalChange(HWND owner);
bool JumpToNoteLinkId(const std::wstring& linkId,
                      const std::wstring& notePath,
                      bool showNotFoundNotice = true,
                      const std::wstring& sourceNotePath = L"",
                      std::optional<size_t> sourcePos = std::nullopt);

bool IsNoteTyping();
bool IsNoteImeComposing();
