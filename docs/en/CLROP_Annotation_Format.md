# `.clrop` PDF Annotation Data

`.clrop` is the JSON file that PDF Note Workspace uses to save annotations for a PDF. Its name denotes the PDF-annotation counterpart of the Classroom-derived `.clro` note extension. It is not a note or the PDF itself, and annotations do not directly rewrite the PDF.

## Keep it with its PDF

When you annotate a PDF, the application places a same-named `.clrop` file in the same folder.

```text
lecture.pdf     original PDF
lecture.clrop   annotation data for lecture.pdf
```

Both files are needed to display and edit annotations again. When moving, renaming, copying, or sharing a PDF, handle its `.clrop` file together with it.

## What the file contains

The current format is version 1 JSON. It contains information for identifying the PDF and annotations grouped by page.

```json
{
  "version": 1,
  "pdf_id": { "path": "lecture.pdf", "size": 123456, "page_count": 12, "sha256": "..." },
  "pages": [ { "page": 0, "items": [ { "type": "text" } ] } ]
}
```

This is a simplified example. Fields vary by annotation type. The application currently writes text, math, text markers and color changes, freehand markers, freehand ink, lines, arrows, waves, note-link markers, and shapes. PDF identification includes more than a path: it can use size, page count, page sizes, and SHA-256 data.

## Do not edit it by hand

Although it is JSON, `.clrop` is application-managed data rather than a hand-authored format. Incorrect fields or a mismatch with its PDF can prevent annotations from being displayed correctly.

Add, edit, and remove annotations in the application. On save, the application writes UTF-8 JSON through its safe-write path. If saving fails, it does not leave the saved `.clrop` file partly rewritten; use the saved-state, restore, and backup features to review pending work.

Do not rename `.clrop` to `.clro`, change its extension to change its type, or edit its contents in a text editor.

For the human-authored note format, see [`.clro` Note Format](CLRO_Note_Format.md). For format selection and moving files, see [File Formats](File_Formats.md).
