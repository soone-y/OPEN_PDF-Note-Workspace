# Using the Application

## Start working

1. Open or select the PDF you want to read in your workspace.
2. Create a note, or add highlights, handwriting, text, or shapes over the PDF as needed.
3. Save your work, then run integrated save when you finish.

To protect original PDFs, the application keeps annotations and work data as separate recoverable data. If you are unsure about a save destination or displayed content, stop making changes and read [Saving and Recovery](Save_and_Recovery.md).

## Choose how to keep your record

The ways to keep a record while reading a PDF have different purposes and storage locations. Choose after checking the following differences.

| Type | What it does | How this application handles it |
| --- | --- | --- |
| **Editing a PDF** | Changes the document itself, such as its text, images, or pages. | This is not part of annotation work. The original PDF is kept unchanged. |
| **Annotating a PDF** | PDF provides an annotation mechanism for placing records such as highlights, handwriting, text, or shapes over the document. | The records are shown over the PDF while you read and work. |
| **Separate-file annotations** | Stores annotations as data separate from the PDF itself. | This application stores them in a same-named `.clrop` file. Keep the PDF and `.clrop` together to display and edit the annotations again. |

Export combines the PDF and its annotations into a single new PDF file, preserving the original PDF. See [File Formats](File_Formats.md) for the PDF/`.clrop` relationship and moving precautions.

With the text annotation tool, click an existing text box to edit it again. A strip inside each of its top and bottom edges is excluded from reediting: 10% of the box height per edge, capped at 31% of one line's height in that box's font. Hovering over a position that allows reediting shows a candidate insertion caret inside the box.

A/B are font-size presets. While reediting, click A or B to apply that preset, including clicking the active one again to return to its size. `Ctrl++` / `Ctrl+-` change the target box and its active size display while keeping the saved A/B values and the inactive display. Selecting a box or applying a theme leaves its font size unchanged.

When the annotation toolbar is too narrow to fit all options vertically, use its wheel scrolling or vertical scrollbar to reach the lower settings. Scrolling over a closed dropdown moves the toolbar. Keyboard navigation also brings the focused setting into view.

## Create a note and save your work

Choose **Create Note** to make a new note. The default `.clro` format is the application's standard note. Choose `.md` or `.markdown` when compatibility with other Markdown tools matters; `.txt` and `.csv` are plain text formats.

In Settings, select Note → Files to save the default format for new notes (`.clro`, `.md`, `.txt`, or `.csv`). Choosing a format from the creation button context menu changes the same default. Existing files are unaffected.

Opening a session and typing without opening a note also creates a new note in this default format.

Delete an empty `.clro` from the file list context menu. After clearing text you entered, save the empty note and wait for saving to finish before deleting it. A newly created empty note can be deleted immediately. A file containing only a BOM also has an empty body; files containing spaces, line breaks, or other body text are retained.

Give the note a short, recognizable name. Opening and editing an existing `.md` or `.txt` file does not change it into a `.clro` file. See [File Formats](File_Formats.md) for the differences between formats.

Work in progress is protected separately while you edit. Use `Ctrl+S`, the Save menu, normal exit, or export to save. Before updating a source file, the application creates a pre-save copy and confirms the write. If saving fails, do not repeatedly overwrite the file; record what is shown and read [Saving and Recovery](Save_and_Recovery.md) or [Troubleshooting](Troubleshooting.md).

Before exporting, the application saves pending changes to notes and annotations. This updates the note files and `.clrop`, but does not change the original PDF. Export does not proceed if saving fails.

Choose an output file other than the original PDF. If an output file with the same name already exists, the application asks whether to overwrite it.

::: details Annotation types in exported PDFs and later editing

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

Use the read-only viewer to read a PDF with its `.clrop` annotations. Export from the main application to combine the PDF and its annotations into a single PDF file.

Normal saving still uses a separate `.clrop` file. Exported PDF annotations are not imported into the application's editable annotation data. Keep the original PDF and `.clrop` for further work in this application.

If an annotation cannot be created, export stops before writing the result.

PNG output also displays native annotations contained in the PDF. **Include annotations** controls the additional separate-file app layer; it does not remove native PDF annotations.

:::

## Finding settings

The sidebar groups settings by where you work: **Environment, PDF, Note, Annotation, Save, and Export**. Settings buttons in the note and annotation panes open the relevant page in the same window.

- Environment: window and shared controls, colors and palettes, file lists, and diagnostics and settings management.
- PDF: view and controls.
- Note: text and display, input and keys, markup, and files. Review or edit additional allowed extensions under **Files**. Restricted PDF, Office, executable, and other reserved types cannot be enabled through this list.
- Annotation: tools and display, input, and a shortcut to the shared palette.
- Save: automatic protection of work in progress and access to backup and recovery.
- Export: defaults for Quick PDF and Quick Note export. Use the export dialog for the current destination, range, and reservations. Its Quick Export settings button opens the same settings window.

The dedicated color picker returns only the confirmed color to the calling editor and does not save the palette.

Switching pages preserves drafts. **Revert this page** affects only the current subpage. **Save settings** commits edits and keeps the window open; **Save and close** closes after saving succeeds. Closing with unsaved edits offers Save, Discard, or Continue editing. Palettes, shortcut edits, and other separately saved changes remain saved when drafts are discarded.

## Center guide before opening a document

While no PDF or note is open, the PDF pane shows numbered usage steps and common actions and shortcuts in the center. Opening a PDF or note hides the guide.

In **Environment → Window and Shared Controls**, choose **Usage and shortcuts**, **Shortcuts only**, or **Hidden**. Both sections appear by default. Use the wheel, scrollbar, or Page Up / Page Down to read a long guide.

The text at the bottom explains that you can choose **Shortcuts only** or **Hidden** and shows where to find the setting. This explanation stays at the bottom while you scroll the guide.

Individual annotation keys come from the current bindings. Saving a change under **Annotation → Input** refreshes the guide. It also includes fixed category, tool and palette-color navigation, saving, search, and PDF zoom operations.

## Startup, PDF view, and export defaults

In Environment → Window and shared controls, turn **Resume the previous work on startup** OFF to open only the workspace on the next launch. Previous records are retained. A file explicitly specified at startup still opens.

**Display when opening a PDF** in PDF → View and controls offers the existing automatic fit, previous position and zoom, first-page width, or entire first page. Existing behavior is the default. Older position records without zoom information use automatic fitting.

CLRO-to-Markdown syntax normalization is configured in the export dialog and starts ON. Select **Use this choice as the default next time** to save it across launches. Without that action, changes do not alter the default used after restarting. Quick Note exports TXT and does not use this setting.

## Rotate a PDF page

Use **View → Rotate** to turn either the current page or all pages clockwise or counterclockwise by 90 degrees, or restore their original orientations. Use the all-pages commands for a PDF distributed sideways. They add the same turn to each page, preserving differences from individual page adjustments. This view setting is temporary and is not written to the source PDF or startup settings. New text, math and shapes are upright on the current screen, then remain attached to the paper as it rotates. Their positions and content orientations are saved in `.clrop`.

The PDF export dialog's **Keep page rotation** checkbox starts ON. It captures each page's view angle when the export setting is added; queued and merged exports retain those angles. Turn it OFF to export in the original PDF orientation. Width and height refer to the selected output orientation. Quick PDF also defaults to ON. Change this checkbox and select **Set as Quick PDF** to save its ON/OFF preference. The angles themselves are not saved; Quick PDF uses the view angles at the time of export. PNG retains the original PDF orientation.

## Full and Lite editions

Dropping files onto either the PDF or note list copies them into the current session without confirmation. Both lists accept the same files and sort them by format.

Drop folders onto the lecture list to copy them as new lectures, or onto the session list to copy them as new sessions in the selected lecture. Folder names and hierarchy are preserved. A lecture folder should contain session folders directly beneath it; drop a folder containing documents directly onto the session list instead.

Source files and folders remain unchanged. Name conflicts, repeated or overlapping inputs, mixed files and folders, and incorrect drop targets reject the entire drop before copying starts. Existing data is never overwritten or merged, and conflicts are not renamed automatically. A read or write failure during copying stops the operation; completed items are retained and the result is shown.

Before importing or copying a total of at least 100 MB, the application asks whether to Continue or Stop. Guidance has three levels: at least 100 MB, 500 MB, or 1 GB. Files inside folders are included in the total. Stop is the default; closing the dialog, pressing Esc, or accepting the default with Enter does not start the operation. MB means 1,000,000 bytes and GB means 1,000,000,000 bytes.

Dropped `.docx` / `.pptx` files are imported in their original format, without automatic PDF conversion, in both editions. In Full, right-click the session and choose **Convert Office files to PDF** to convert files that do not yet have a matching PDF together. Imported originals remain available after restarting. The conversion menu also lets you select external Office files for direct conversion. Lite requires the standard edition or a separately prepared PDF for Office-to-PDF conversion.

- The Full edition can use the bundled conversion features.
- The Lite edition is a smaller viewing and annotation environment and does not include Full-only conversion features.

Neither edition has external communication features.

## Read-only viewer

`readonly_viewer.exe` lets you compare multiple PDFs, notes, and documents in tabs. It displays annotations from a same-named `.clrop` file in the PDF's folder. See [Read-only Viewer](Read_Only_Viewer.md) for its purpose and reading controls. Use `pdf_note_workspace.exe` for editing work.
