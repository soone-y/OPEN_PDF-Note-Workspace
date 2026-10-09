# Using the Application

## Start working

1. Open a PDF and add notes or annotations as needed.
2. Save your work, then run integrated save when you finish.

To protect original PDFs, the application keeps annotations and work data as separate recoverable data. If you are unsure about a save destination or displayed content, stop making changes and read [Saving and Recovery](Save_and_Recovery.md).

## Startup and window checks

Starting the application again from the same package location brings its existing main window forward. Packages at different locations can run independently, but opening the same PDF, note, or other protected document concurrently is restricted.

If startup checks find an application at another location without a displayed main window, you can confirm a normal shutdown request. Request delivery and actual process exit are checked separately. If exit is not confirmed within 10 seconds, it is reported as unconfirmed; the application may be saving or awaiting confirmation and is not forcibly terminated. Older versions without a process-specific shutdown endpoint are reported by the check, but no shutdown request is sent to them.

If this application's main window remains unavailable for at least 15 seconds, it makes up to three spaced display recovery attempts. If the window does not return, a silent notice independent of the main window offers "Retry display", "Try normal exit", and "Keep waiting". The notice closes automatically when the main window returns. Waiting or closing the notice suppresses repeat notices for the same incident. Minimization or a brief hide does not cause shutdown. A missing window alone does not permit forced termination when safe preservation and recovery of work cannot be verified. This monitor does not detect an unresponsive application whose main window remains visible.

## Three ways of working with a PDF

Several related features have different purposes and storage locations. This application distinguishes them in the following order.

| Type | What it does | How this application handles it |
| --- | --- | --- |
| **Editing a PDF** | Changes the document itself, such as its text, images, or pages. | This is not part of annotation work. The original PDF is kept unchanged. |
| **Annotating a PDF** | PDF provides an annotation mechanism for placing records such as highlights, handwriting, text, or shapes over the document. | The records are shown over the PDF while you read and work. |
| **Separate-file annotations** | Stores annotations as data separate from the PDF itself. | This application stores them in a same-named `.clrop` file. Keep the PDF and `.clrop` together to display and edit the annotations again. |

When you need a PDF with annotations for sharing or submission, export a new PDF without changing the original. See [File Formats](File_Formats.md) for the PDF/`.clrop` relationship and moving precautions.

## Annotations in exported PDFs

The main application's combined PDF export puts the PDF and its separate-file annotations into one new PDF. Writing is saved as standard PDF annotations, so it remains accessible as annotations after export.

Flattening makes the writing part of the page content rather than separate annotations. The main application does not flatten its exported writing. Compatible PDF applications can list, move or remove these annotations. Editing their text and geometry depends on the application and annotation type.

| Writing | PDF annotation |
| --- | --- |
| Text highlights | `Highlight`, with text ranges and color. |
| Handwriting, free markers, lines, arrows and waves | `Ink`, with stroke points and appearance. Arrows and waves remain ink strokes, not native line annotations. |
| Text boxes | `FreeText` by default, retaining Unicode contents and a vector appearance. When the appearance cannot be preserved, export uses `Stamp`. |
| Rectangles and squares | `Square`; circles and ellipses use `Circle`. |
| Math, other shapes and note-link markers | `Stamp`, preserving appearance. Expressions or link IDs remain in annotation contents; note markers do not become clickable PDF links. |

Turning off **Prefer FreeText** exports text boxes as image-based `Stamp` annotations. This option applies only to text boxes; there is no PDF-annotation/flattened-output switch.

The bundled read-only viewer uses a different output method: its annotated-PDF export flattens the writing from `.clrop` into page content. Export from the main application when you need standard PDF annotations.

Normal saving still uses a separate `.clrop` file. Exported PDF annotations are not imported into the application's editable annotation data. Keep the original PDF and `.clrop` for further work in this application.

If an annotation cannot be created, export stops before writing the result. Text recoloring requires changing the original page text and remains unsupported for annotated-PDF export; selecting affected pages stops output without creating a file.

If the source PDF already contains native PDF annotations, export at 1x. Non-unit scaling stops before replacing the destination because existing annotation geometry and appearances cannot yet be transformed reliably. Scaling remains available when the source PDF has no native annotations and only separate app annotations are added.

PNG output also displays native annotations contained in the PDF. **Include annotations** controls the additional separate-file app layer; it does not remove native PDF annotations.

## Full and Lite editions

- The Full edition can use the bundled conversion features.
- The Lite edition is a smaller viewing and annotation environment and does not include Full-only conversion features.

Neither edition has external communication features.

## Read-only viewer

`readonly_viewer.exe` is a small application for viewing PDFs. Use `pdf_note_workspace.exe` for editing work.
