# File Formats

| Extension | Purpose |
| --- | --- |
| `.clro` | The application's standard note format; UTF-8 text. |
| `.md` / `.markdown` | Markdown-compatible notes; existing files are never renamed automatically. |
| `.tex` | Existing TeX source files. |
| `.txt` / `.csv` | Plain-text notes; `.csv` is not processed as a spreadsheet. |
| `.clrop` | Annotation data associated with a PDF, not a note. |

## Edit temporary paths in startup settings

`tempExternalLectureDirs` in `pdf_note_workspace_setup.json`, beside the exe, lists temporary paths. When the app saves this list, it writes one path per line. Existing single-line arrays remain readable.

`tempExternalLecturePriorities` is an array pairing registered paths with priorities from 0 to 9999. It is saved together with the path list. New paths receive the first available value from 5000 upward. Earlier settings without this field remain readable. Edit or clear priorities under **Settings > Environment > File Lists > Lecture Order**.

To edit the list in Notepad or another text editor, close the app, copy the settings file as a backup, then edit the existing array and save as UTF-8. This is a field excerpt; keep the other settings intact.

```json
  "tempExternalLectureDirs": [
    "C:\\Course materials\\Surgery",
    "D:\\Lectures\\Medicine"
  ]
```

Enclose each path in double quotes and write each path separator `\` as `\\`. Separate entries with commas, with no comma after the last entry. Use `[]` for an empty list. Relative paths use the exe's folder as their base. The edited list is loaded on the next startup.

## Format examples

`.clro` is UTF-8 text that people write. For example, a file named `research-notes.clro` can contain the following Markdown text:

```markdown
# Reading notes

- Check the figure on page 3.
- Review the annotations next time.
```

`.clrop` is JSON that the application saves to associate a PDF with its annotations. The following short example is for recognizing the structure. Actual files contain information that depends on the PDF and the annotation types used.

```json
{
  "version": 1,
  "pdf_id": {
    "path": "lecture-material.pdf",
    "size": 123456,
    "page_count": 1,
    "page_sizes_pt": [[595, 842]],
    "sha256": "(value used to identify the paired PDF)"
  },
  "pages": [
    { "page": 0, "items": [] }
  ]
}
```

This `.clrop` example explains the shape of the data; it is not a template for creating or editing an annotation file by hand. Add and change annotations in the application. For the detailed notation and fields, see [`.clro` Note Format](CLRO_Note_Format.md) and [`.clrop` PDF Annotation Data](CLROP_Annotation_Format.md).

When annotating a PDF, the application creates a same-named `.clrop` file in the PDF's folder. Both the PDF and its `.clrop` file are required to view or edit annotations again. Move or share them together, and do not edit the `.clrop` manually or change its extension.

For the role and notation of `.clro`, see [`.clro` Note Format](CLRO_Note_Format.md). For the PDF relationship and JSON role of `.clrop`, see [`.clrop` PDF Annotation Data](CLROP_Annotation_Format.md).
