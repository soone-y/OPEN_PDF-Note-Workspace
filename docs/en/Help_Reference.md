# Help Reference

Use this reference while working to check where a command is, what to select, and what it changes. For a first complete task, read [Using the Application](Using_the_App.md). Detailed notation examples belong in the [Note writing guide](Note_Writing_Guide.md); failed saves and recovery belong in [Saving and Recovery](Save_and_Recovery.md).

> **About these instructions** — Screen labels, layout and steps may differ slightly depending on your version, settings and display environment. Use the descriptions as a guide and check what appears on screen as you proceed. Before saving, deleting or exporting, check the target files, destination and confirmation details.

## Find the operation you need

| Purpose | Reference |
| --- | --- |
| Prepare and organize materials | [Files and locations](#files-and-locations), [conversion and blank PDFs](#convert-materials-or-create-a-blank-pdf) |
| Read comfortably | [PDF view and navigation](#pdf-view-and-navigation) |
| Write on a PDF | [Annotation tools and colors](#annotation-tools-and-colors) |
| Write a separate note | [Notes and formatting](#notes-and-formatting) |
| Keep or recover work | [Saving and recovery](#saving-and-recovery) |
| Share results | [Output](#output) |
| Change behavior | [Settings](#settings), [keyboard controls](#keyboard-controls) |
| Investigate a failure | [Checks and help](#checks-and-help), [Troubleshooting](Troubleshooting.md) |

## Choose the application and edition

This is a local application for PDFs, notes, and annotations. It has no external communication features. While you edit, it does not write directly to the original PDF; it uses recoverable working data.

The Full edition can convert Office files to PDF locally with bundled features. Lite does not include the conversion runtime. Neither edition uses Microsoft Office or an online conversion service.

`readonly_viewer.exe` is a small viewer for multiple PDFs, notes, and bundled documents, without a browser engine. It displays saved annotations from a same-named `.clrop` file in the PDF's folder. See [Read-only Viewer](Read_Only_Viewer.md) for an introduction and usage. Use `pdf_note_workspace.exe` to edit.

Use the file list and outline on the left, tabs at the top, and the document pane on the right to compare documents. Press `Ctrl+F` to search, `Ctrl+B` for focus mode, and `Ctrl+1` / `2` / `3` / `4` for Reading, Raw, Hex, and Diagram views. Drag tabs to reorder, move to another window, or detach. Supported Markdown tables and Mermaid diagrams render locally; unsupported diagrams retain their source text. The **Licenses** menu opens the bundled guide and lets you browse the original notices.

## Files and locations

### Create a lecture and a session

A workspace is the root folder you select for your materials. A lecture groups sessions, and a session contains the PDFs and notes used together. Choose **File > Create > Lecture**, enter a name and confirm. Select that lecture, then use **File > Create > Session**. Canceling creates nothing. For example, create “Statistics” and its “Session 01”, then put that day's PDF and notes there. Generic terminology can label these as upper and lower items.

### Import files or folders

Select a session and use **File > Import > File** (`Ctrl+O`), or drop files onto either the PDF or note pane. Files are copied into the current session and classified by format; originals remain. Dropping starts without an individual confirmation, except for the size check below. Dropping an Office file or image copies its original format rather than converting it.

To import a complete lecture, use **File > Import > Lecture** or drop onto the lecture list. Prepare a folder with session folders directly inside it. For a folder containing materials directly, select the destination lecture and use **File > Import > Session** or drop onto the session list. The folder name and hierarchy are retained in the copy.

Conflicting destination names, duplicate or overlapping inputs, mixed files and folders, or an incorrect drop target stop the batch before it starts. Import does not overwrite, merge, or invent new names. A read/write failure during copying stops processing and reports the result while retaining completed items.

At 100 MB or more, the application asks before starting. Notices distinguish 100 MB, 500 MB and 1 GB, including files within folders; MB means 1,000,000 bytes. The default is Cancel; closing, Esc or default Enter does not begin the copy.

### Create a note or remove an empty note

Select a session and use **Create Note** or **File > Create > Note**. Enter a name and choose a format; the note opens for typing. The default is `.clro`. Change it to `.clro`, `.md`, `.txt` or `.csv` under **Settings > Note > Files** or the creation button's context menu. This changes future creation only. Typing with a session open and no note selected also creates a note in that default format.

Use `.clro` or `.md` for formatted notes, `.txt` or `.csv` for plain text. CSV has no spreadsheet processing. Delete an empty `.clro` from the file-list context menu. After clearing an existing note, save and wait for completion before deleting it. A newly created empty note can be deleted immediately. A BOM-only file counts as empty; spaces and line breaks count as body text and are retained.

### Rename, move, organize, or inspect files

Right-click an item in the lecture, session, PDF or note list and choose **Copy name** to copy its actual folder or file name. File names include the extension; display decorations such as **External:** are omitted.

**Reload list** rescans the directory shown in that list to reflect items added or removed outside the app. It is also available from the list's blank area. In the PDF and note lists, it refreshes both file lists for the current session. Open PDFs, notes and unsaved edits are retained.

**Edit > Rename > Note**, or the file list context menu, shows the full filename including its extension. PDF names must keep `.pdf` (case-insensitive). Standard note extensions are `.txt`, `.csv`, `.md`, `.markdown`, `.note`, `.tex`, `.icr` and `.clro`. Other extensions require confirmation that they will be treated as plain text, like txt. Approved extensions are saved for the workspace, and files with the same extension become note candidates. Renaming does not convert the contents. Declining keeps the draft and original file. Missing extensions, invalid filenames, and restricted PDF, image, Office and executable formats are rejected.

Use **Settings > Note > Files > Additional allowed note extensions** to view, add or remove entries separated by commas or spaces. Clear the field to remove the additional permissions. Restricted PDF, image, Office and executable formats remain excluded from this permission list.

In the app's folder picker, long paths omit intermediate folders while retaining the destination folder. Hover over the path bar to see the complete path or use **Copy path** to copy it. Clicking a displayed path component still opens the corresponding folder.

Select a PDF and choose **Edit > Rename > PDF** to rename it with its annotation and history files. You may also rename the matching note. Keep the `.pdf` extension, including upper-case `.PDF`. Note renaming updates links referencing the note; it does not convert the contents.

Use **Edit > Move file** for the selected PDF or note and choose a destination. The application handles pending work first; annotation and history files follow a PDF. Moving changes the actual file location, whereas importing makes a copy. Canceling the destination picker does not move it. Review the destination list after completion. Use **Organize** for the selected session when arranging its PDF and note files into the configured folders; check the proposed target before proceeding.

### Use an external folder through Temporary Paths

Choose **File > Temporary Paths > Add external folder** to register a folder without copying it into the workspace. Registration follows an access check and remains available after restart. Its sessions can be used for editing, saving, renaming, moving and organizing; these operations affect the actual external files. “Temporary” does not mean the Windows temporary directory. Remove the entry through the Temporary Paths menu to remove the registration, leaving its folder and documents intact. Failed registrations can be retried through [Retry unsaved settings](Save_and_Recovery.md#retry-unsaved-settings).

Do not edit `.clrop` in a text editor. Keep it beside its same-named PDF when copying or sharing. See [File Formats](File_Formats.md) for format choices and [annotation data](CLROP_Annotation_Format.md) for safe handling.


## Convert materials or create a blank PDF

### Convert Office documents

In Full, use the **Tools** command for experimental DOCX/PPTX conversion to select external Office documents. For documents already imported into a session, right-click the session and choose **Convert Office files to PDF** to process those without a PDF yet. Dropping a DOCX/PPTX copies it without converting.

Conversion uses bundled LibreOffice locally, without Microsoft Office or an online service. You can continue working during background conversion. Canceling or closing the application, including Windows shutdown requests, stops the conversion process. Original documents remain. Review the generated PDF: fonts, shapes and equations can look different. Lite imports Office originals but does not include the conversion runtime.

### Convert an image

Import a PNG/JPG/JPEG, select it in the PDF list and use its context menu's image-to-PDF command, or the corresponding **Tools** command. It creates a one-page PDF based on the image dimensions and resolution. Dropping alone does not convert, and the original image is unchanged.

### Create a blank PDF

Select a session and use **Tools > Create blank PDF**. Choose paper size, orientation and 1–500 pages. After choosing **Create**, select the destination and filename. The new PDF opens so you can write with pen, text or shapes. Canceling either the options or destination step creates no PDF. Writing is stored as separate annotations, just as for other PDFs.


## PDF view and navigation

Open a PDF from the current session's PDF list. Select the note separately to read and write alongside it. **View > Close** closes only the chosen PDF or note; **File > Close Files** closes both after protecting edits. See [Protecting open originals](Save_and_Recovery.md#protecting-open-originals) before moving files externally.

### Zoom, page navigation and reading direction

Use **View > Page** for first, previous, next, last or a page number (`Ctrl+G`). Numbers start at 1. **View > Zoom** lets you enter a percentage, return to 100% (`Ctrl+0`), or fit the current page (`Ctrl+E`), which adjusts both zoom and position. Ctrl+wheel changes zoom. These affect viewing rather than PDF contents.

Under **View > Scroll**, choose top-to-bottom, bottom-to-top, right-to-left or left-to-right page progression, and single/continuous page display. A single page helps concentrate on one sheet; continuous display lets you read across page boundaries. **View > Left column** frees space by hiding workspace lists. Readability is a display aid for low-contrast PDF text, not an edit to its original text.

### PDF page rotation

Use **View → Rotate** to turn either the current page or all pages clockwise or counterclockwise by 90 degrees, or restore their original orientations. Use the all-pages commands for a PDF distributed sideways. They add the same turn to each page, preserving differences from individual page adjustments. Page rotations are saved in the PDF's last-view record. On reopening, rotation is restored before zoom and position; fit-width and fit-page use the rotated dimensions too. The source PDF orientation is unchanged. New text, math and shapes are upright on the current screen, then remain attached to the paper as it rotates. Their positions and content orientations are saved in `.clrop`.

Rotations use four clockwise quarter-turn states, stored in the workspace's `__pdf_note_workspace__/__tmp__/pdf_view_positions.txt`. Older records without rotations use zero additional rotation. Rotation data with a different page count is not applied. Read or write failures preserve the existing record and failed saves are reported.

The PDF export dialog's **Keep page rotation** checkbox starts ON. It captures each page's view angle when the export setting is added; queued and merged exports retain those angles. Turn it OFF to export in the original PDF orientation. Width and height refer to the selected output orientation. Quick PDF also defaults to ON. Change this checkbox and select **Set as Quick PDF** to save its ON/OFF preference. The angles themselves are not saved; Quick PDF uses the view angles at the time of export. PNG retains the original PDF orientation.

### PDF scrolling

Touchpad scrolling preserves fine input deltas. **Touchpad scroll stabilization** in Basic settings defaults to ON. In PDF and rendered note views it reduces small reversals during continuous scrolling. Sustained movement in the opposite direction takes effect. Turn it OFF to bypass this suppression. High-resolution wheels that send fine input deltas may also receive stabilization. The note's raw text editor uses standard Windows scrolling.

The annotation, lecture, session, PDF and note lists, and the application's file picker, accumulate fine scroll input separately for each list and move by whole rows, following the Windows wheel setting. Ordinary annotation-list updates retain your browsing position; an action that reveals a newly added annotation brings that item into view.

For PDF scrolling, arrow keys move by a fixed amount. With the PDF focused, hold `W` for up, `A` for left, `S` for down, or `D` for right to scroll smoothly. Hold two keys to move diagonally; releasing the keys stops movement. These PDF controls work with Vim controls on or off. While editing PDF text, the keys enter text.

## Annotation tools and colors

Open a PDF, choose a tool in the annotation pane, choose its subtype and color/width/opacity options, then work on the page. Options below the visible pane can be reached with its wheel or vertical scrollbar; keyboard focus also scrolls settings into view. Images and read-only previews do not offer the same editing operations as a PDF. **Settings > Annotation > Tools and display** changes creation defaults; use an existing annotation's editor to change that item.

Annotations are stored in the PDF's same-named `.clrop`, leaving the PDF original unchanged. Ctrl+S saves work; output creates a separate shareable result. Save failures and recoverable drafts are covered by [Saving and Recovery](Save_and_Recovery.md).

### Select, pan and magnify

**Select** drags a text selection in PDF text; Ctrl+C copies it. This creates no annotation. A scanned page needs a free marker for its image-only lettering. Use the annotation list when selecting a saved annotation.

**Pan** drags the displayed page without moving its annotations. **Ctrl+drag on an annotation** moves that annotation instead. **Magnifier** enlarges the area you click, with circle, square or horizontal shape, size and zoom settings. It is a viewing aid and is not included in saved annotations or output.

### Text boxes

Choose Text and click on the page, then type. Enter adds a line. Ctrl+Enter or clicking outside the box confirms editing. Esc also confirms and ends editing; use Undo to reverse a change. Finish IME composition before commands. Click an existing box with Text to reedit it. Near its top and bottom edges, an excluded strip prevents reediting; an insertion caret indicates an eligible position. Each excluded strip is 10% of box height, capped at 31% of a line in its font.

Choose font, size, text color, background and wrapping. Ctrl+drag changes position. A/B are saved creation-size presets: clicking either applies it, including clicking an active preset again. Selecting a box or applying a theme alone does not resize it.

While editing or selecting a text box, use `Ctrl++` / `Ctrl+-` to step through font sizes: 1pt steps from 4 to 24pt, 2pt from 24 to 32pt, 4pt from 32 to 48pt, 8pt from 48 to 96pt, then 16pt up to 192pt, with 200pt as the largest preset. A size between presets moves to the next preset in the requested direction. Use Properties to enter an individual size, including decimals, from 4 to 200pt.

The toolbar temporarily displays the actual font family and size of the box being edited or selected. **Current** identifies this temporary size; it leaves the saved A/B sizes and A4-relative setting unchanged. Changes made with `Ctrl++` / `Ctrl+-` update the size display too. Selecting a box does not apply settings or overwrite the A/B creation sizes. Explicitly choosing a font family or size applies only that attribute to the box. Choosing a color preserves the editing caret and selection.

### Markers and text color

| Tool | Operation and result |
| --- | --- |
| Text marker | Drag across PDF text, then release to place bands along its lines. Requires PDF text data. |
| Underline | Select text in the same way to place a narrow emphasis beneath it. |
| Text color | Select text to display its glyph shapes in the chosen color without changing PDF text data. |
| Free marker | Hold and draw a broad freehand band; release to finish. Useful over scans and diagrams. |
| Line, arrow or wave marker | Drag between two points for geometric emphasis; segmentation follows the line tools below. |

Text markers follow columns in vertical PDF text and rows in horizontal PDF text. A vertical underline-style marker appears on the right of the column. Direction is inferred from character positions and spacing; different directions and substantially different sizes, such as ruby text, remain separate. Characters with insufficient direction evidence retain individual bounds. Use a free marker for lettering stored only in an image.

In the main application, **Text color** displays the original glyph shapes in the selected color. When text data is present in the PDF page, this also helps read text matching its background or covered by a shape. It does not restore lettering stored only in an image or text that has been deleted.

Choose **Translucent** or **Multiply** in the annotation panel for shape fills and outlines, text markers, underlines, and free markers. Shapes initially use Translucent; markers use Multiply. Multiply keeps dark text dark. The selected mode applies to subsequent creations. To change an existing annotation, use **Display mode** in its properties. Existing saved annotations retain their previous display. This display choice does not change PDF or PNG output.

Text color preserves filled, stroked, and filled-and-stroked lettering. Partially covered edge pixels may retain some of the original color.

### Pen, lines, arrows and waves

With Pen, hold the mouse button and draw; release to finish one stroke. Choose width, color, opacity and handwriting correction. Pen suits handwritten notes; a free marker suits broader emphasis. Undo reverses a stroke, Ctrl+drag moves it, and the eraser or annotation list deletes it. Handwriting correction can reshape a stroke; inspect its result and Undo if it does not match your intention.

Choose Line, Arrow or Wave and drag from start to end, then release. A line divides or connects elements; an arrow points to its endpoint and can use one or two heads; a wave provides a wavy emphasis. Adjust width, color, opacity and supported line style.

While drawing a line, arrow or wave, press Ctrl or Enter to place a segment and continue drawing. When bending an arrow with Ctrl, intermediate segments become ordinary lines; the final segment receives the arrowhead when you release the mouse button. Intermediate segments keep the dash pattern, color, width and opacity. Enter keeps the selected line type and arrowhead setting for each segment.

### Shapes and rotation

Choose a shape and its appearance, then drag from one corner of its bounds to the opposite corner and release. Very small bounds do not create a shape.

| Shape | Use and difference |
| --- | --- |
| Rectangle / Square | Box a passage or table; a square keeps equal dimensions. |
| Ellipse / Circle | Circle an object or key term; an ellipse allows different horizontal and vertical spans. |
| Triangle / Equilateral triangle | Add a diagram element or attention mark; the equilateral variant keeps its proportions. |
| Diamond | Distinguish a diagram element or a decision in a flow. |
| Arc (parenthesis) | Group lines with an open bracket-like curve. It does not fill an enclosed area. |

Choose **Arc (parenthesis)** for an open curve. Drag its bounding rectangle to set the vertical span and bow depth. Drag right for `(` or left for `)`. Use the rotation handle after placement or **Alt+click** for further rotation. As with straight lines, choose solid or dashed style, width, color and opacity. Fill, black/white boundary and blend controls are hidden for this shape.

The shape **Line style** list offers **Solid**, **Dashed**, **Boundary (black)** and **Boundary (white)** in one control. Solid and Dashed use the shape color; black and white boundaries are solid. Filled shapes retain their fill color with the selected boundary drawn over it; outline-only shapes use a black or white outline. Boundaries use normal compositing with the shape's width and opacity. Choose the creation default in the annotation panel or change an existing shape in its properties. The boundary is also included in PDF and PNG output. Existing shapes retain their appearance.

After placing a shape, a temporary rotation handle appears, connected by a line to its fixed center pivot. Drag the handle to rotate the whole shape. Use **Alt+click** on an existing shape to show the same handle; release Alt before dragging if desired. Releasing the mouse commits one undoable rotation. Esc during a drag cancels that drag and hides the handle. Esc or another canvas click also hides the handle; the click continues its normal action. **Ctrl+drag** retains its move behavior. New rotated ellipses use the ordinary ellipse tool and this rotation operation; existing rotated-ellipse data retains its appearance.

### Math on the PDF

Open a PDF and select **View > Bottom pane (temporary) > MathBox input**. Type an expression such as `\frac{a}{b}` and inspect the preview. Drag the preview onto a PDF page and release to place it; releasing outside a page does not place it. **Add as LaTeX** or Ctrl+Enter places it in the current view's center. **Add as Markup** is also available.

Use the annotation list's **Edit** command to revise its source, color or position. MathBox is saved in `.clrop`; equations written in a note are saved in the note instead. Math rendering is experimental, so inspect exported results too. See the [note writing guide](Note_Writing_Guide.md) for supported expressions.

### Find, revise, duplicate or delete an annotation

Select an annotation in the annotation list and right-click. **Properties** inspects it; **Edit** opens editable fields for its type, including source text, coordinates, color, width, opacity and text or shape options. Apply changes and inspect the page; canceling discards that edit.

The same menu offers jumping to the annotation, copying its text, opening a note link, duplicating, bringing forward, sending backward and deleting. Text copying requires a text-bearing item; link opening requires a link marker. Stacking changes concern annotations on the same page. The jump command navigates the display; Ctrl+drag changes placement.

With Eraser, click or drag over annotations to delete the touched annotation as a unit. It does not erase PDF text or images. Use list deletion when objects overlap. **Clear all** removes this PDF's separate-file annotations, leaving original PDF contents and native annotations intact. Undo (`Ctrl+Z`) reverses recent annotation operations; Redo is Ctrl+Y. Saving commits deletions to `.clrop`; recovery after saving or restarting requires retained backup/recovery data.

### Colors, display and defaults

The active tool's color controls new annotations. The shared palette supplies reusable colors; custom colors can be stored there. Ctrl+Up / Ctrl+Down cycles the active tool's palette color. A single-color picker returns its confirmed color without saving palette changes. Changing creation settings does not recolor every existing annotation: use the item editor for those changes.

Show/hide annotations changes their visibility without deleting them (`Ctrl+Shift+A`). It is separate from output's Include annotations option. Text backgrounds and wrapping aid readability without changing the PDF beneath them. The application display blend choice and the saved boundary settings have different export effects, described under Shapes and Markers above.


## Notes and formatting

Click the open note and type. Markdown-family notes (`.clro`, `.md`, `.markdown`) can show formatting while retaining editable source. A heading such as `# Review`, a list such as `- Check page 3`, and `**important**` provide a simple first note. Plain `.txt` and `.csv` retain literal text; CSV is not interpreted as a table.

### Source and structured display

The edited line or selection displays source. Tables switch as a whole, and a block equation switches over its equation range. Leaving that range restores structured display. Turning rendering off shows all source. Vim Normal/Insert choices are described under [keyboard controls](#keyboard-controls). Display wrapping and lower editing space do not insert line breaks or blank lines into the saved note.

Use the bottom pane's **Headings** mode to inspect note structure, **Note** to return to editing, **MathBox input** to place PDF equations, or **Status assist** for viewing and input-state controls. Temporary pane changes use View; save the next-launch default in Settings.

### Input and formatting palette

Choose **Writing guide** to open the bundled [Note writing guide](Note_Writing_Guide.md) in the read-only viewer. It works without an open note and preserves the input field and note contents.

Use the lower-left input and formatting palette to insert text with **Apply** or Enter. Half-width and full-width spaces are preserved, and no trailing newline is added automatically. With the input field empty, formatting applies to the selected note text. With text in the input field, insertion occurs immediately after the selection. If both are empty, the note stays unchanged and a notice explains what to do.

Bold, italic and strike use Markdown `**`, `*` and `~~`; headings use `#` through `######`. Supported auxiliary attributes add text color, background, underline, size and other display properties. Headings, bullets and quotes inserted within a paragraph receive the necessary boundary newlines. Formatting generation requires a Markdown note. For `.txt`, `.csv` and `.tex`, formatting is declined and the input field is kept; clear the formatting controls to insert ordinary text.

While IME composition is active, Enter commits composition; apply after committing. The Apply button also waits for composition to finish. The input field clears only after insertion is verified, and stays intact when no note is open or formatting cannot be applied. Note undo and redo restore both insertion and selected-text formatting.

### Markup, math and links

Use normal Markdown for headings, emphasis, lists, quotes and tables. Auxiliary tags add supported attributes: `<u>word</>` underlines, `<char=ff0000>word</>` colors text, and `<font=Consolas>word</>` changes a range's font. Rendering affects appearance; the source remains editable. The [Note writing guide](Note_Writing_Guide.md) is the central reference for examples, full-width CLRO syntax, attributes and combinations.

Write `$x^2$` for an inline equation or `$$...$$` for a block equation. While editing, its source is visible. Rendering is experimental; check the output you plan to share. For an equation placed over PDF rather than in a note, use MathBox input.

Choose **Link** (PDF link) in the input palette to start, click a position in the note, then click its matching position on the PDF. Two points complete the operation. Choosing Link again ends the pending mode and removes incomplete PDF markers. A note anchor such as `<l=topic-a>explanation</>` identifies the link. Open a link marker from the annotation list to return to the note. Use Markdown-family notes; `.txt` and `.csv` do not interpret these links. Keep referenced notes and the PDF/CLROP pair together when moving work. The writing guide explains IDs and cases without a selected PDF. Exported PDF markers preserve appearance and identification text, not an active jump to a note.


## Search and working memo

### Search

Open Search from its menu or Ctrl+F. Enter a query, choose one range (current lecture, current session or whole workspace) and one or more targets (Notes, PDF, Annotations), then choose Search. Include registered external lectures when needed.

Space-separated terms use AND; `OR` or `|` uses OR; `"..."` requests a phrase. For example, `heart valve` looks for both words and `heart OR lung` for either. Options control normalization of full/half-width forms, kana and separators, and case-insensitive matching.

Select a result and press Enter or double-click to open its document and location. Visited results are colored. The context menu can open a result in the read-only viewer. Delete lowers a result in the list; it does not delete the source file. Open memo opens the same workspace memo used elsewhere.

Results are added in batches, and you can work in other windows while searching. Loading a complex document can still briefly delay a response to input.

Choose **Hide options** to fold the search conditions and guidance away and enlarge the results area. Conditions stay selected; **Show options** restores them. The folded window can be made shorter. **Keep above main window** keeps search in front of this app's main window while leaving keyboard focus where you are working. These presentation choices last until the search window closes.

While the search window has focus, press Ctrl+C during a search or choose **Stop (Ctrl+C)** to cancel it. The window stays open and retains the query and results found so far. When no search is running, Ctrl+C keeps its normal copy behavior. Ctrl+C in the main note or memo does not cancel a search.

### Memo

One plain-text memo per workspace holds search terms, source locations and working notes. Tools > Workspace memo and the search screen's Open memo button open the same editor. Closing search or settings leaves it open. Markdown and rich formatting are not supported.

Ctrl+S while typing and the Save button save only this memo. Closing it and exiting the application also save automatically. Failure cancels closing or switching and keeps the text. The original is `__pdf_note_workspace__/__memo__/workspace_memo.txt` inside the workspace (UTF-8, maximum 1 MiB). Settings presets do not include it.

Input is limited to 1 MiB of UTF-8 with LF line endings. Oversized input or pastes are rejected as a whole, without truncating or changing the previous text. Edited text is saved with LF endings; an unedited original retains its exact line endings and BOM.

You may edit the original directly. Conflicting external changes stop saving instead of being overwritten. Reload original first archives your draft in `backups/`, then reads the external version. Previous originals are also retained there. A durable `workspace_memo.recovery` draft is restored the next time you open the memo after an interrupted run. Do not delete this folder's data after a failure or crash. Old global/search memo files are not automatically converted or deleted; copy any needed contents into the new memo manually.

## Saving and recovery

| Purpose | Action and result |
| --- | --- |
| Protect ongoing edits | Automatic work protection keeps recoverable drafts separately. Switching files/sessions/lectures does not write originals. |
| Commit work | **Save > Save Work** (`Ctrl+S`) creates pre-save backups, verifies safe writes, then updates notes and `.clrop`. It does not rewrite the PDF. Normal exit and pre-output saving use this protection. |
| Save only the current note | The current-note save command excludes PDF annotation integration. |
| Inspect failed or pending work | **Save > Review Save Status** lists protected work. Choose its recovery operation after reviewing the target. Discarding a draft leaves the saved original unchanged. |
| Recover a saved file | **Save > Saved file backups > Restore** opens the backup list. Select a candidate and inspect Details for the original, time and full destination path before restoring. |
| Remove a backup | Use Delete in the same backup submenu. Select only an unneeded copy and read the confirmation. This removes that recovery source, not the original or working draft. |
| Restore viewing/list state | **Settings > Restore** selects PDF position, lecture recent order, or session last-open PDF/note state. It is distinct from file-content backup restoration. |

Before restoration, review protected work so an old original and a newer draft are not confused. Recent editing mistakes use Undo; it is not a substitute for backups across saving, closing and restarting. Keep recovery data after an error. [Saving and Recovery](Save_and_Recovery.md) is the central procedure for protected originals, failed saves, conflicting changes, defer/save-as-new recovery, retrying settings and data locations. [Troubleshooting](Troubleshooting.md) gives safe first steps.


## Output

Use **Output > Output Dialog...** to choose annotated PDF, selected-page PDF, single-page PNG, TXT, Markdown, or HTML. Set the destination folder and output file name before output; useful defaults are supplied and may be changed. With no queue, **Export** creates the current output alone. Add each desired setting to the queue, then select **Export all queued** to create all queued results and show one result list at the end. Use **Export selected** to create just one result: success removes that reservation and keeps the dialog and remaining queue open; failure or overwrite cancellation retains the reservation. Closing the dialog discards unexecuted reservations. Select a reservation to change its destination or file name and choose **Update reservation**, or move, remove, or clear it. The same destination cannot be queued twice, and the open original cannot be selected as output. Existing output files require an explicit overwrite confirmation. The application safely integrates current edits before output.

Hover over the destination folder field to see the complete path, or select the field and use Ctrl+A, Ctrl+C to copy it. TXT markup removal defaults to OFF, including Quick Note; previously saved settings remain in effect.

### Merge queued PDFs

Use **Move up** and **Move down** to change the queue order. Both separate output and merging run from top to bottom. **Merge queued PDFs** is available only with at least two reservations, all of them annotated PDFs or selected-page PDFs. Choose one destination; the merged PDF preserves each reservation's page order, crop, scale, and annotation options. Individual reservation destinations are not written, and the results dialog lists only the merged file. Success clears the completed queue while keeping the dialog open. Destination or overwrite cancellation and output failure retain the queue. PNG or note reservations prevent merging.

After output, **Open with OS** in the results dialog reveals the selected output in Explorer. PDFs can open in the read-only viewer; other formats open with their associated application.

**Browse** opens the app's file picker, where you can navigate to a destination folder and enter a file name together. Selecting a listed file copies its name into the input. Choosing a destination does not export or overwrite anything. Quick PDF, Quick Note and other output-path prompts use the same picker. **OS Standard Dialog** switches to the Windows save dialog and retains the folder and entered name. When **Use Windows standard file/folder selection** is enabled in General settings, output destination selection opens the Windows save dialog directly. Canceling leaves the destination unchanged. The folder hierarchy display remains a separate browsing and operation feature.

The app's file and folder picker shows folders whose names include terms such as **course** or **lecture** in an upper **Suggestions** pane. When choosing a workspace, the current workspace is also suggested if it appears in the listing. Below a divider, **All entries (name order)** lists every folder followed by every file, including the suggestions. Either pane leads to the same location; selecting the same location in both panes for a multiple selection returns it once. Temporary path registration, opening files and choosing output destinations share this display. Locations without suggestions and the drive list use only the normal listing.

**Quick PDF** uses the annotated-PDF settings saved with **Save as Quick PDF** in the dialog. **Quick Note** always creates TXT; save its text, math, comment-line, and markup choices with **Save as Quick Note**. **View quick output settings** opens a compact, immediately closable summary of both current quick-output configurations. Output always creates a separate result and does not overwrite the open original.

### Text color output options

For PDF **Text color output**, choose **Replace with highlight** (default), **Overlay on transparent background**, or an overlay with an automatic or specified background. Enter a specified color as `#RRGGBB`, for example `#FFFFFF`. Overlays use the original glyph outlines and positions in a Stamp annotation, preserving the page content and searchable text. Transparent overlays may leave edges of the original lettering visible; filled backgrounds also cover diagrams and rules within the selection. Automatic color estimates the surrounding representative color and does not reconstruct photos or gradients. If glyph outlines cannot be obtained, output stops rather than silently switching to a highlight. Each reservation retains its mode and color, including merges; **Set as Quick PDF** saves them for Quick PDF. PNG text-color rendering is unchanged.

### Selected-page PDF and single-page PNG

In the output dialog, select the source PDF and format. Page numbers start at 1: `1,3,5-7` outputs pages 1, 3, 5, 6 and 7 in that order. `4a,1n` includes separate-file annotations on page 4 and omits them on page 1. Repeated pages remain in the specified order. A range runs from the smaller to the larger page number.

To crop a page, use a rectangle such as `1[0:0:360:540]`: the four values are left, bottom, right, top in PDF points (72 points per inch), measured from the lower-left of the page. Use a rectangle within the page with left < right and bottom < top. These are not screen pixels; review cropped results before sharing. The annotation suffix follows the rectangle, for example `1[0:0:360:540]a`.

PNG accepts one page number. Choose Standard (144 DPI), High detail (288 DPI), Small (72 DPI), or custom pixel dimensions with preserved aspect ratio and a 32-million-pixel limit. Higher resolution increases size and processing time. Choose whether to include separate-file annotations and a PDF-style or screen-like appearance. Native PDF annotations remain visible: Include annotations controls only the separate-file layer. PNG uses the original PDF orientation. Open the result to inspect thin strokes, translucent markers, math and colored text. Protected PDFs may restrict export.

### Note export choices

TXT is useful for literal text; markup removal is initially OFF. Choose whether to retain comments and how math is represented. Quick Note always uses TXT with its saved defaults. Markdown retains markup; CLRO-to-Markdown normalization of full-width syntax is initially ON. This converts recognized syntax names and symbols, retaining body text, code, math, link targets and attribute values. The option does not apply to TXT or HTML. HTML provides a separate formatted result. Review comments before sharing, and inspect the actual target format rather than assuming all other tools render it identically.

### PDF annotation output

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


## Settings

Open **Settings > Settings dialog**, then choose a group and page on the left. Moving between pages retains the draft. **Save** applies changes and stays open; **Save and close** applies and closes. **Revert this tab** returns only the current page's unapplied draft. Follow the save/discard/continue choice when closing with edits. Palette, shortcut and priority editors use their own saving controls; the main dialog's discard does not undo independently saved changes.

| Group | Pages and effects |
| --- | --- |
| Environment | Window/shared controls, Bottom pane, Colors/palette, File lists, Diagnostics/settings management |
| PDF | View/controls: opening position and zoom, scrolling and other viewing behavior |
| Note | Text/display, Input/keys, Markup, Files: font, background, rendering, Vim, syntax choices, creation format and allowed extensions |
| Annotation | Tools/display and Controls/key bindings: creation appearance, correction, marker display and direct tool keys |
| Save | Automatic/integrated save and Backup/recovery: work protection and entry points for recovery |
| Export | Quick PDF and Quick Note defaults; the output dialog sets this run's destination, page range and queue |

Turning off **Resume previous work at startup** opens only the workspace next time without deleting previous records. Explicit startup filenames still open. **PDF opening view** offers automatic adjustment, previous position/zoom, first-page width or first-page fit; older records without zoom use automatic adjustment. Notes' font, background, wrapping and bottom space affect viewing without adding text to the original. Check existing notes after changing markup interpretation.

### Center guide before opening a document

While no PDF or note is open, the PDF pane shows numbered usage steps and common actions and shortcuts in the center. Opening a PDF or note hides the guide.

In **Environment → Window and Shared Controls**, choose **Usage and shortcuts**, **Shortcuts only**, or **Hidden**. Both sections appear by default. Use the wheel, scrollbar, or Page Up / Page Down to read a long guide.

The text at the bottom explains that you can choose **Shortcuts only** or **Hidden** and shows where to find the setting. This explanation stays at the bottom while you scroll the guide.

Individual annotation keys come from the current bindings. Saving a change under **Annotation → Controls and shortcuts** refreshes the guide. It also includes fixed category, tool and palette-color navigation, saving, search, and PDF zoom operations.

### Appearance, note input and markup choices

Under **Note > Text and display**, choose a font and size and inspect the preview. Font customization can assign separate faces for Latin/symbols, Japanese and digits. A note's explicit `<font=...>` ranges remain separate from this display default. Background and grid spacing change the editing surface rather than inserting colored or ruled content. Use formatting tags to color a particular passage.

**Note bottom space** offers fixed pixels, a font-size ratio or a pane-height ratio. The default is 32px at 100%; zero removes it. It leaves space below the caret without adding blank lines to the saved note. See [Note writing guide](Note_Writing_Guide.md#space-below-the-last-line) for rendered/source differences.

**Note > Markup** sets heading font/color, bold, underline and left bar, mark font/color and custom-tag style presets. It changes how supported syntax appears, whereas the input palette inserts syntax into the note. Review existing notes after changing it; the writing guide supplies the notation examples.

**Annotation > Tools and display** sets creation defaults for text, markers, lines, shapes, pen, eraser and magnifier. Lines, arrows and waves may share color and width. Tool visibility choices are On, Off (disabled) or Off (hidden); individual direct-key bindings remain independent, while read-only and save-time restrictions still apply. Handwriting correction offers automatic, hold, smooth and off modes, with line/geometric/automatic styles and geometric-fill or always-fill choices. Inspect corrected strokes and Undo unwanted results. Edit an existing item through the annotation list instead of changing creation defaults.

**Environment > Colors and palettes** manages reusable annotation colors. Selecting a color for one tool is distinct from editing palette contents. Select a screen theme in Window and shared controls; create a custom theme by duplicating the current one, then edit and save it separately. Palette/theme saves are not reversed by canceling the main settings dialog.

### List order, opening behavior and new names

Lecture ordering offers priority, name and recent use; session ordering offers ascending numbers, descending numbers or names. Numeric ordering suits Session 1, 2 and 10. The priority rules below explain temporary paths. Sorting does not rename real folders.

Open PDF and note files individually from their lists. **Files inside a session** chooses separate PDF/note folders or a shared session folder. Changing the setting does not move existing files; run **Organize session files** to align them with the chosen layout. New-session numbering offers count plus one or maximum number plus one; gaps in existing numbers can give different suggestions. It changes new-name suggestions, not existing names. **Note > Files** controls the note naming rule, new format and additional allowed extensions; renaming does not convert content.

### Automatic protection interval

Under **Save > Automatic protection and saving**, annotation work protection offers Off, 5 seconds, 30 seconds, 1 minute or 5 minutes. It adjusts periodic internal protection, not timed writes into the PDF original. Off stops this periodic protection; boundary protection and explicit saving are separate. Notes are protected on line movement, focus departure, before saving and after 6–20 seconds idle. Use Ctrl+S to commit originals and check save status before sharing.

### Presets, priorities and diagnostic settings

Save a `.pnssettings` preset to carry workspace settings, palettes, annotation shortcuts, selected/custom themes and lecture ordering to another environment. It excludes PDFs, notes, annotations, memo, caches, logs, history and backups. Loading replaces settings after confirmation and archives existing settings; a failed application restores the prior state. Unrelated custom themes are retained. A preset is not a document backup.

In **Environment > File lists > Lecture order**, set priorities from 0–9999 or right-click an entry and choose Change priority. Lower numbers come first; ties use names and unassigned entries come last. Priority ordering combines lectures and temporary paths. Name/recent ordering puts temporary paths first in priority order. New temporary paths take the first free value from 5000 upwards; existing values remain. Free a value if 5000–9999 are all occupied. This editor saves immediately. Lecture priorities belong to the workspace; temporary-path priorities belong to the application across its workspaces.

Developer mode exposes diagnostic commands; it is not needed for normal editing. Some logging settings take effect on next startup. Use **Check sizes** to inspect management logs and generated files without deleting them; see [Troubleshooting](Troubleshooting.md). Failed settings save notices retain input and may offer [Retry unsaved settings](Save_and_Recovery.md#retry-unsaved-settings).

### Status assist

A subtle dedicated header distinguishes this pane from a note pane, and it scrolls vertically when needed. Items follow the order PDF → note display → fonts and background → input → analysis details. Settings appear beside their related state. Tinted rows can be clicked to change a setting; read-only or unavailable rows are not tinted. Source font identifies the font used for source display, not the note's text.

The page-number and zoom displays drawn on the PDF can be turned on or off independently. Enabling Vim controls immediately moves focus to the note's Normal mode. During IME composition, commit or cancel the composition before toggling Vim controls. Related controls appear only while Vim controls are enabled. The Normal-mode source caret-line preference is available only while rendering is on. Note click shows whether a click enters Insert or keeps Normal. Changes affect the display immediately without changing the note text; preferences are retained when their controls become unavailable. In Insert mode, the caret line and selected ranges are shown as source text. With rendering off, the entire note is source text.

Display setting identifies the chosen display mode. Current display reports the actual structured view, source editing ranges, or temporary source-view fallback; clicking this row does not change the setting. Operation target identifies the area receiving commands. Keyboard focus identifies the actual focused control. Input mode reports the note's Normal, Insert or Visual state, or search input. During Normal note navigation, both target and focus read Note. Moving to search, PDF or a file list updates the focus label. With Vim controls off, the input mode reads Standard input. Set the next-launch default from Settings.

Use **View > Bottom pane (temporary)** to switch the current display while retaining the saved default. Saving or applying the bottom-pane settings page later stores and activates the dialog selection, including reapplying the same saved value. Merely opening or closing the page, or saving only another page, preserves the temporary display. A failed save retains the saved default, temporary display and editable draft. Reloading content or starting again uses the saved default.

Changes saved or applied in the settings dialog are persistent settings. Items with a separate save mechanism, such as the palette, retain that mechanism.

## Keyboard controls

### Annotation tool bindings

Open **Settings → Annotation → Controls and shortcuts → Open shortcut overview and bindings** to review and edit bindings in one panel. Common fixed keys appear beside the editable list. Direct tool bindings are independent of annotation bar visibility, disabled state, and order, including hidden buttons. Image, read-only preview, and save-time editing restrictions remain in effect. Text entry takes priority.

New defaults use `Ctrl+1–9` for Select, Pan, Magnifier, Text, Marker, Free marker, Pen, Line, and Shape. `Ctrl+0` stays reserved for 100% zoom. Existing custom settings are retained. Use **Add unassigned Ctrl+1–9**, review the list, and save to add only unused combinations.

Choose a main key and the Ctrl, Shift, and Alt checkboxes, then choose a tool. Top-row digits and numpad keys are separate choices. The composed chord, duplicate or reserved-key conflicts, and any overridden common action appear immediately. **Press keys to assign** records an actual combination while suppressing its original action. Esc cancels recording only.

Update the list or select another row to retain the draft, then save. Cancel and failed saves preserve the saved bindings. Saving an empty list disables individual bindings. Earlier category, Ctrl+Alt+digit, and numpad bindings remain editable.

Ctrl+Alt+arrows cycle tool categories and subtypes; Ctrl+Up/Down cycles palette colors. Fixed cycling keys are not replaced by the individual shortcut editor. Text fields and IME composition take priority over annotation commands.

### Common action shortcuts

| Key | Main application action |
| --- | --- |
| `Ctrl+G` | Go to a page number |
| `Ctrl+E` | Fit the current page to the PDF pane, adjusting both zoom and position |
| `Ctrl+O` | Import a file into the current session |
| `Ctrl+Shift+Q` | Close the focused PDF or note; prefer the PDF when neither area has focus |
| `Ctrl+Alt+Shift+Q` | Close both PDF and note |
| `Ctrl+Shift+A` | Show/hide annotations |
| `Ctrl+Shift+Left` / `Ctrl+Shift+Right` | Rotate the current page left/right 90 degrees |
| `Ctrl+Shift+0` | Reset the current page's view rotation |
| `Ctrl+Alt+M` | Open memo |
| `Ctrl+Alt+E` | Open the output dialog |
| `Ctrl+Alt+P` | Quick PDF output |
| `Ctrl+Alt+T` | Quick note output (text) |

Page navigation, annotation visibility, and rotation apply to the PDF outside text fields. Open, Close, Memo, and Output also work from text fields. IME composition and AltGr text input take priority. These operations reuse the menus' save protection, destination selection, confirmation, and cancellation paths.

Existing custom annotation bindings take priority on overlapping chords; the settings reference and central guide mark those overrides. These are default common-action keys, not an editor for binding arbitrary non-annotation actions. See [Read-Only Viewer](Read_Only_Viewer.md) for its supported keys.

### Vim-style note controls

With Vim controls enabled, press `Esc` or `Ctrl+[` to enter Normal mode while keeping keyboard focus in the note. Finish IME composition first. Normal disables the note's IME; pressing `i` returns to Insert and restores IME input. Use `h` for left, `j` for down, `k` for up, and `l` for right; arrow keys move in the same directions. Pressing `h` at the start of the note keeps input in the note.

In Normal mode, `Esc` clears a selection or pending command and keeps Normal mode active. Press `i` to resume typing. To change panes, use `Ctrl+h` for the file list or `Ctrl+k` for the PDF.

From the PDF, `Ctrl+j` moves focus to the note at its current caret position. With Vim controls enabled, it enters Normal; press `i` to start typing. With Vim controls off, it uses standard input.

The note search field opened with `/` supports IME input. Closing it with `Esc` returns keyboard focus to the note in Normal mode.

## Checks and help

### Operation and write checks

**Help > Checks** groups Operation and write checks, Asset presence check, PDF Information, and Note Information.

Open **Help > Checks > Operation and write checks** to see all 16 built-in checks and the last result and time for each current target. Results are **No record**, **Failed**, **Succeeded**, **Check itself failed**, or **Canceled**. A missing target or PDF conversion in Lite reports **Check itself failed**. **Details** explains the operation, failure stage, target and retained data locations.

**Current PDF read check** checks the PDF header, opening with PDFium and page presence on disk, without writing or rendering every page. **Current note read/write check** reads the entire note on disk, checks read/write access and tests a new file in the same directory for writing, reopening, comparison and deletion. It does not modify the original or unsaved edits. Missing files or directories report **Check itself failed**, with the cause in Details. In separate-folder mode, **Organize session files** creates pdf and note folders; checks do not create them.

Checks cover the current PDF and note directories and direct PDF and note placement in the session root separately. Rows with the same destination share the latest result. Checks do not change the file layout setting. Like **Asset presence check**, the buttons start with **Details** and **Refresh** and end with **Close**.

Select a row and choose **Run check**. Write checks create a dedicated new file, write and read it back, compare its contents and remove it. Existing settings and documents are preserved. PDF conversion uses a fixed DOCX demo in a dedicated temporary area and validates the generated and placed PDFs and byte equality. Shared LibreOffice cache writes are not a check item. Success applies to the shown time, target and operation; it does not guarantee future saves or conversion fidelity for every document.

Conversion data stays inside `__pdf_note_workspace__/__tmp__/lo` in the selected workspace. The conversion temporary-parent write check targets `__tmp__`. If it cannot be written, conversion stops without falling back to AppData or the Windows temporary folder. Writes outside the application folder are allowed for the selected workspace, folders registered through **Temporary Path**, and explicitly chosen save or export destinations. **Temporary Path** registers external folders; it does not select the conversion temporary area.

The limit is shared by all items. Five checks with unchanged results trigger a five-minute cooldown. First results, changed results and cancellations do not count. The remaining time is shown below the list. During cooldown, untested items and previous **Failed** results after confirmed corrective changes can use a shared allowance of two additional checks. The remaining allowance is shown. Closing the window keeps the limit; restarting the application resets it.

Actual normal-use writes of settings, temporary saves, recovery data, logs, PDFs and notes, and PDF conversion results also update the latest status. Details identifies normal use or a manual check and shows the actual operation target. Normal-use success covers that operation only; it does not establish completion of the manual check's reopen, comparison or deletion steps. Skipped saves and cancellations do not add successes, and normal-use updates do not affect the check count or cooldown.

Latest results are saved in the workspace's `__pdf_note_workspace__/__log__/write_checks.log`. Saving removes oldest results above 64 records or 64 KiB while preserving the newest entry. A removed target's result is shown as No record. If saving fails, results remain on screen with an unsaved notice. **Retry saving results** writes the unsaved results to the log again without repeating checks or counting toward the check limit. Opening or refreshing the window does not run checks. Refresh after changing targets before running another check.

### Asset presence check

Presence checks are ten seconds apart, with the remaining time shown on the refresh button. Closing and reopening retains the previous snapshot and cooldown; restarting resets them. There is no automatic recheck during cooldown. Confirmed absence is **Missing**; permission, drive or other inspection errors report **Check itself failed**, with the cause in Details. Unselected targets show **Not selected**.

Theme files that could not be inspected remain listed as failures, and the theme listing also reports failure. Content viewing distinguishes current absence, read errors and files exceeding the display limit. Read errors include the error cause; viewing does not modify files.

The header shows the last check time in local time, and the list shows changes since the preceding check of the same workspace: **Present → missing**, **Missing → present**, **Newly observed**, **Type changed**, or **No change**. The first check, the first check after a workspace switch, and checks after restarting show **No previous check**. If either inspection could not determine presence, the comparison is **Cannot compare**, rather than a claim of disappearance. Previously observed themes remain listed when missing and are checked again on later refreshes. Details shows the current and previous check times and states. Results and comparisons are held only for the current application run and are not saved to a file. If the overall inspection fails, the previous results and check time are retained.

**Details** in both Asset presence check and the operation and write checks shows status, paths, then additional information. Hover over an abbreviated path to see its full value, or use **Copy paths** to copy the complete paths.

Operation and write checks report unreadable records separately from unsaved results, with the cause in Details. A failed record read is not treated as no record. Unsaved results remain on screen; **Retry saving results** retries saving without running the check again.

Normal startup-setting reads, workspace listings and PDF opens also update results automatically. First results and changes are recorded immediately. Repeated identical reads update the display while record writes are grouped at ten-second intervals. Pending saves are shown and also saved with the next record write, workspace switch or exit. Page rendering does not repeatedly record results or start extra checks, and normal reads do not count toward the manual check limit.

Open **Help > Checks > Asset presence check** for a separate window with the same description, report list and action-button arrangement as the operation and write checks. Rows show item, presence, type and target name. Use **Details** or double-click a row to inspect its location; **Copy path** copies the complete path. Outside cooldown, presence is checked when a new window opens and when you choose **Refresh presence**, without changing settings or documents. Bringing an already open window to the front does not recheck presence, and there is no automatic monitoring. File content viewing remains available; **Open with OS** shows a selected folder in Explorer. Save or load presets through **Settings > Presets**.

Use **Check sizes** to inspect the management directory and its logs, temporary work, recovery data and backups in the background. Sizes, observed file counts and size changes are separate from presence checks. Original documents outside the management directory are excluded; no files are created, changed or deleted. Parent rows include child rows. Cancellation and inspection errors show partial results. See [Inspect log and generated-file sizes](Troubleshooting.md#inspect-log-and-generated-file-sizes) for scope and limits.

### Use the bundled help

Open Help and choose a category on the left, then read its body on the right. Enter in the search field searches instead of closing the window. Next searches the displayed category; Search all includes other categories. Open detailed documentation launches the bundled reference in the read-only viewer; if documentation is missing it opens the viewer alone. Help does not connect to an external site.

Before reporting an issue, note the selected PDF/note, save-status message, operation time and destination. Follow [Troubleshooting](Troubleshooting.md) before deleting or replacing any recovery data.
