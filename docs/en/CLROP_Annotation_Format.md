# Save, Move and Share PDF Annotation Data

`.clrop` stores markers, handwriting, text, math, shapes and note links placed over a PDF. The application writes it separately, leaving the original PDF unchanged. The human-authored note format is `.clro`.

## Keep it with its PDF

The annotation file is beside its PDF, with the same base name and a different extension.

```text
lecture.pdf     original PDF
lecture.clrop   annotations for this PDF
```

Opening the PDF overlays its matching annotations. Both files are needed to display and edit them again. Copy, move or share both together. In-app rename and move commands handle related files. Before organizing with Explorer, close the displayed file in the application and preserve the matching names.

## Protecting an open PDF and checking its pairing

The main application and the read-only viewer prevent external applications from writing, deleting, renaming or replacing a PDF while it is open. Close every view of that PDF before editing it in another application. If an external application already holds write access, finish that operation before opening the PDF here. Ordinary read-only access remains available.

A same-name `.clrop` is a candidate. The application compares file size, page count and original page dimensions. The main application offers choices on a mismatch and also checks SHA-256 before the first staging save of loaded annotations. The read-only viewer also checks content when SHA-256 is recorded; it hides mismatched annotations and displays a notice. Legacy files without SHA-256 use the available size and page information. The viewer never rewrites pairing information.

Renaming only the PDF externally leaves its old `.clrop` undiscovered during normal opening. Keep their names aligned, or explicitly start the candidate search described below. Candidates are never applied automatically.

## Find PDF candidates

Open `Tools → Find PDF / CLROP candidates` and choose a search mode:

- **Compare within a directory** examines PDFs and CLROPs immediately inside the chosen directory. It also identifies CLROPs without an adjacent same-name PDF.
- **Find PDFs for a CLROP** takes one selected CLROP and a separately selected PDF directory. Other CLROPs in that directory are not investigated.

Start the search to list candidates ranked by SHA-256, structure and size, structure, size, page count, or name. Each piece of evidence has its own column. Structure means page count and ordered page dimensions. Changed-size PDFs can remain structure candidates. Identical copies remain separate rows.

Hashing is restricted to candidates with a recorded hash and compatible byte counts, or an unknown recorded size. Unchecked differs from different. Read failures, invalid data, cancellation and count, time or read limits leave explicit unchecked results. Subdirectories and stored paths never expand the search automatically.

Select a candidate and choose the read-only preview. A separate window temporarily overlays its annotations, including weaker candidates, with a permanent unconfirmed-pairing banner. Inspect placement and page correspondence. A match or preview does not apply the pairing: the tool does not rename, move or update either input. Preview sessions are not saved in normal viewer settings.

## Correct an annotation's page

The annotation list retains entries for pages absent from the current PDF, marked with their page number and `[out of range]`. After reviewing the PDF pairing and loading the annotations, right-click the list entry, open its properties and enable editing. Change **Page** using one-based numbering. A new destination must exist in the current PDF.

Apply records one undoable annotation update. IDs, content, style and PDF-space coordinates are retained. Adjust coordinates in the same properties window if the destination page has different dimensions. Leaving an existing out-of-range number unchanged lets you edit other properties while retaining that annotation. Page insertion or deletion never automatically relocates or deletes annotations.

Inspect the result, then use normal saving to commit it to `.clrop`.

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

JSON structure, numeric formatting and line endings, defaults, compatibility, marker display modes, arc and boundary fields, shape rotation and coordinates are collected in the public [Data structures reference (Japanese)](https://soone-y.github.io/OPEN_PDF-Note-Workspace/introduction/core/data_schemas.html#1-%E6%B3%A8%E9%87%88%E4%BF%9D%E5%AD%98%E3%83%95%E3%82%A1%E3%82%A4%E3%83%AB-clrop---classroom-pdf-annotation). They are not prerequisites for ordinary use.

See [File Formats](File_Formats.md) for the note/annotation comparison and [CLRO Note Format](CLRO_Note_Format.md) for human-authored text.
