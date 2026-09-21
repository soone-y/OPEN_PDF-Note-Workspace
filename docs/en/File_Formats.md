# File Formats

| Extension | Purpose |
| --- | --- |
| `.clro` | The application's standard note format; UTF-8 text. |
| `.md` / `.markdown` | Markdown-compatible notes; existing files are never renamed automatically. |
| `.tex` | Existing TeX source files. |
| `.txt` / `.csv` | Plain-text notes; `.csv` is not processed as a spreadsheet. |
| `.clrop` | Annotation data associated with a PDF, not a note. |

When annotating a PDF, the application creates a same-named `.clrop` file in the PDF's folder. Both the PDF and its `.clrop` file are required to view or edit annotations again. Move or share them together, and do not edit the `.clrop` manually or change its extension.

For the role and notation of `.clro`, see [`.clro` Note Format](CLRO_Note_Format.md). For the PDF relationship and JSON role of `.clrop`, see [`.clrop` PDF Annotation Data](CLROP_Annotation_Format.md).
