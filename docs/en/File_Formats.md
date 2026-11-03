# File Formats

| Extension | Purpose |
| --- | --- |
| `.clro` | The application's standard note format; UTF-8 text. |
| `.md` / `.markdown` | Markdown-compatible notes; existing files are never renamed automatically. |
| `.tex` | Existing TeX source files. |
| `.txt` / `.csv` | Plain-text notes; `.csv` is not processed as a spreadsheet. |
| `.clrop` | Annotation data associated with a PDF, not a note. |

## Use an external folder

Register [Temporary Paths](Help_Reference.md#use-an-external-folder-through-temporary-paths) to work with external files without copying them. Saving, renaming and moving affect the actual registered files. Removing a registration leaves originals intact. Ordinary use does not require startup JSON editing. Field names and manual editing are in the [technical reference (Japanese)](https://soone-y.github.io/OPEN_PDF-Note-Workspace/introduction/core/data_schemas.html#3-起動設定ワークスペース設定).

## Format examples

`.clro` is UTF-8 text that people write. For example, a file named `research-notes.clro` can contain the following Markdown text:

```markdown
# Reading notes

- Check the figure on page 3.
- Review the annotations next time.
```

For `.clrop`, use the application's annotation tools rather than a text editor. [Data structures (Japanese)](https://soone-y.github.io/OPEN_PDF-Note-Workspace/introduction/core/data_schemas.html) contains JSON examples.

When annotating a PDF, the application creates a same-named `.clrop` file in the PDF's folder. Both the PDF and its `.clrop` file are required to view or edit annotations again. Move or share them together, and do not edit the `.clrop` manually or change its extension.

For the role and notation of `.clro`, see [`.clro` Note Format](CLRO_Note_Format.md). [Choose what to share](CLROP_Annotation_Format.md#choose-what-to-share) explains which files to send or export for editing in this application, reading in the companion viewer, or reading in another PDF application.

## Choose a format

- New structured notes: use `.clro`.
- Notes exchanged with Markdown tools: use `.md` or `.markdown`; application-specific formatting may differ elsewhere.
- Literal text: use `.txt`, or `.csv` to inspect/edit CSV as ordinary text without spreadsheet processing.
- Existing TeX source: open `.tex` as source.
- PDF annotations: let the app manage `.clrop` beside the PDF.

Existing files keep their extension unless you explicitly rename them. Renaming does not convert their contents. `.clro` and Markdown notes share supported formatting; plain text does not interpret Markdown, math or note links. Images imported as PNG/JPG/JPEG remain images until you request one-page PDF conversion.
