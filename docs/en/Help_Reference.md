# Help Reference

## About this application

This is a local application for PDFs, notes, and annotations. It has no external communication features. While you edit, it does not write directly to the original PDF; it uses recoverable working data.

## Basic flow

1. Select a workspace in a writable location.
2. Open and edit a PDF or note.
3. Save and integrate at a suitable stopping point.

## Saving and recovery

Changes are kept in a stage first and applied to original data by integrated save. A backup is made before integration. If a save fails, do not alter the PDF, note, or `__resource__`; read [Saving and Recovery](Save_and_Recovery.md) and [Troubleshooting](Troubleshooting.md).

## Notes and annotations

- `.clro` and `.md` are text formats for notes.
- `.txt` is plain text without formatting.
- `.clrop` is annotation data associated with a PDF. Do not edit or rename it by hand.

See [File Formats](File_Formats.md) for details.

## Full and Lite editions

The Full edition can convert Office files to PDF locally with bundled features. Lite does not include the conversion runtime. Neither edition uses Microsoft Office or an online conversion service.

## Read-only viewer

`readonly_viewer.exe` is a viewer for PDFs and bundled documents. Use `pdf_note_workspace.exe` to edit.
