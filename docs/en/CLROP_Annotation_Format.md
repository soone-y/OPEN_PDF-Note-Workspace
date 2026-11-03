# Save, Move and Share PDF Annotation Data

`.clrop` stores markers, handwriting, text, math, shapes and note links placed over a PDF. The application writes it separately, leaving the original PDF unchanged. The human-authored note format is `.clro`.

## Keep it with its PDF

The annotation file is beside its PDF, with the same base name and a different extension.

```text
lecture.pdf     original PDF
lecture.clrop   annotations for this PDF
```

Opening the PDF overlays its matching annotations. Both files are needed to display and edit them again. Copy, move or share both together. In-app rename and move commands handle related files. Before organizing with Explorer, close the displayed file in the application and preserve the matching names.

## What the file contains

It retains text and math source, strokes and marker ranges, shapes, colors, widths, opacity, positions and orientations. It also identifies the associated PDF: renaming this data file is not a way to transfer annotations to another PDF.

See [Annotation tools and colors](Help_Reference.md#annotation-tools-and-colors) for creation and editing. Edits are protected while you work; Ctrl+S, normal exit and pre-output saving write `.clrop`. Switching files or sessions alone does not commit originals. Complete Ctrl+S before sharing. [Saving and Recovery](Save_and_Recovery.md) covers failures.

## Choose what to share

| Recipient's purpose | Files and result |
| --- | --- |
| Continue editing in this application | PDF and same-named `.clrop`; also retain referenced notes and their relationships for note links |
| Read with the companion viewer | Saved PDF and `.clrop` in the same folder; open the PDF in the [viewer](Read_Only_Viewer.md) |
| Read with another PDF application | Create a new annotated PDF through the main application's [Output](Help_Reference.md#output); note-jump links do not transfer |

Use a current compatible application for editing. Older versions can stop when encountering a newer format, shape or rotation field. Application display modes differ from PDF/PNG output, so open the actual result before passing it on.

## Do not edit it by hand

The contents are UTF-8 JSON, but consistent geometry and PDF identification are required. Hand edits and extension changes can cause display errors or wrong associations. Add, change and delete annotations through the application. Do not rename `.clrop` to `.clro` to edit it as a note.

After a save failure, retain originals and protected work and follow [Saving and Recovery](Save_and_Recovery.md#important-notes). Keep annotation and management data while investigating.

## Technical format reference

JSON structure, numeric formatting and line endings, defaults, compatibility, marker display modes, arc and boundary fields, shape rotation and coordinates are collected in the public [Data structures reference (Japanese)](https://soone-y.github.io/OPEN_PDF-Note-Workspace/introduction/core/data_schemas.html#1-注釈保存ファイル-clrop---classroom-pdf-annotation). They are not prerequisites for ordinary use.

See [File Formats](File_Formats.md) for the note/annotation comparison and [CLRO Note Format](CLRO_Note_Format.md) for human-authored text.
