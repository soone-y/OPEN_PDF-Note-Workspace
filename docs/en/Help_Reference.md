# Help Reference

## About this application

This is a local application for PDFs, notes, and annotations. It has no external communication features. While you edit, it does not write directly to the original PDF; it uses recoverable working data.

## Basic flow

1. Select a workspace in a writable location.
2. Open and edit a PDF or note.
3. Save and integrate at a suitable stopping point.

## Saving and recovery

Folders registered through **Temporary Path** support organizing, renaming and moving files within their sessions. These operations affect the actual files in the external folder. A PDF's annotation and history files follow it; when renaming a PDF, you can choose to rename the matching note too. Undo and Redo for the most recent file operation are available during the current application session.

Changes are protected internally while you edit. Save Work, normal exit, and pre-output processing create a backup before safely saving to the original file. Switching keeps protected work without writing the original. If a save fails, do not alter the PDF, note, or `__pdf_note_workspace__`; read [Saving and Recovery](Save_and_Recovery.md) and [Troubleshooting](Troubleshooting.md).

## Notes and annotations

- `.clro` and `.md` are text formats for notes.
- `.txt` is plain text without formatting.
- `.clrop` is annotation data associated with a PDF. Do not edit or rename it by hand.

For shape outlines, choose **Outline**, then select **Solid** or **Dashed** in the line style control. To change an existing shape, use its properties. Dash patterns are retained when saving and exporting.

While editing or selecting a text box, use `Ctrl++` / `Ctrl+-` to step through font sizes: 1pt steps from 4 to 24pt, 2pt from 24 to 32pt, 4pt from 32 to 48pt, 8pt from 48 to 96pt, then 16pt up to 192pt, with 200pt as the largest preset. A size between presets moves to the next preset in the requested direction. Use Properties to enter an individual size, including decimals, from 4 to 200pt.

The toolbar temporarily displays the actual font family and size of the box being edited or selected. **Current** identifies this temporary size; it leaves the saved A/B sizes and A4-relative setting unchanged. Changes made with `Ctrl++` / `Ctrl+-` update the size display too. Selecting a box does not apply settings or overwrite the A/B creation sizes. Explicitly choosing a font family or size applies only that attribute to the box. Choosing a color preserves the editing caret and selection.

While drawing a line, arrow or wave, press Ctrl or Enter to place a segment and continue drawing. When bending an arrow with Ctrl, intermediate segments become ordinary lines; the final segment receives the arrowhead when you release the mouse button. Intermediate segments keep the dash pattern, color, width and opacity. Enter keeps the selected line type and arrowhead setting for each segment.

See [File Formats](File_Formats.md) for details.

## Full and Lite editions

The Full edition can convert Office files to PDF locally with bundled features. Lite does not include the conversion runtime. Neither edition uses Microsoft Office or an online conversion service.

## Search

Results are added in batches, and you can work in other windows while searching. Loading a complex document can still briefly delay a response to input.

Choose **Hide options** to fold the search conditions and guidance away and enlarge the results area. Conditions stay selected; **Show options** restores them. The folded window can be made shorter. **Keep above main window** keeps search in front of this app's main window while leaving keyboard focus where you are working. These presentation choices last until the search window closes.

While the search window has focus, press Ctrl+C during a search or choose **Stop (Ctrl+C)** to cancel it. The window stays open and retains the query and results found so far. When no search is running, Ctrl+C keeps its normal copy behavior. Ctrl+C in the main note or memo does not cancel a search.

## Read-only viewer

`readonly_viewer.exe` is a small viewer for multiple PDFs, notes, and bundled documents, without a browser engine. It displays saved annotations from a same-named `.clrop` file in the PDF's folder. See [Read-only Viewer](Read_Only_Viewer.md) for an introduction and usage. Use `pdf_note_workspace.exe` to edit.

Use the file list and outline on the left, tabs at the top, and the document pane on the right to compare documents. Press `Ctrl+F` to search, `Ctrl+B` for focus mode, and `Ctrl+1` / `2` / `3` / `4` for Reading, Raw, Hex, and Diagram views. Drag tabs to reorder, move to another window, or detach. Supported Markdown tables and Mermaid diagrams render locally; unsupported diagrams retain their source text. The **Licenses** menu opens the bundled guide and lets you browse the original notices.

## File names and locations

Right-click an item in the lecture, session, PDF or note list and choose **Copy name** to copy its actual folder or file name. File names include the extension; display decorations such as **External:** are omitted.

**Reload list** rescans the directory shown in that list to reflect items added or removed outside the app. It is also available from the list's blank area. In the PDF and note lists, it refreshes both file lists for the current session. Open PDFs, notes and unsaved edits are retained.

**Edit > Rename > Note**, or the file list context menu, shows the full filename including its extension. PDF names must keep `.pdf` (case-insensitive). Standard note extensions are `.txt`, `.csv`, `.md`, `.markdown`, `.note`, `.tex`, `.icr` and `.clro`. Other extensions require confirmation that they will be treated as plain text, like txt. Approved extensions are saved for the workspace, and files with the same extension become note candidates. Renaming does not convert the contents. Declining keeps the draft and original file. Missing extensions, invalid filenames, and restricted PDF, image, Office and executable formats are rejected.

Use **Settings > General > Save > Additional allowed note extensions** to view, add or remove entries separated by commas or spaces. Clear the field to remove the additional permissions. You can also edit the `additionalNoteExtensions` array in `workspace.json` (for example, `[".log", ".memo"]`) while the app is closed. Restricted formats remain excluded even when entered directly in the configuration file.

In the app's folder picker, long paths omit intermediate folders while retaining the destination folder. Hover over the path bar to see the complete path or use **Copy path** to copy it. Clicking a displayed path component still opens the corresponding folder.

## Output

For PDF **Text color output**, choose **Replace with highlight** (default), **Overlay on transparent background**, or an overlay with an automatic or specified background. Enter a specified color as `#RRGGBB`, for example `#FFFFFF`. Overlays use the original glyph outlines and positions in a Stamp annotation, preserving the page content and searchable text. Transparent overlays may leave edges of the original lettering visible; filled backgrounds also cover diagrams and rules within the selection. Automatic color estimates the surrounding representative color and does not reconstruct photos or gradients. If glyph outlines cannot be obtained, output stops rather than silently switching to a highlight. Each reservation retains its mode and color, including merges; **Set as Quick PDF** saves them for Quick PDF. PNG text-color rendering is unchanged.

Use **Output > Output Dialog...** to choose annotated PDF, selected-page PDF, single-page PNG, TXT, Markdown, or HTML. Set the destination folder and output file name before output; useful defaults are supplied and may be changed. With no queue, **Export** creates the current output alone. Add each desired setting to the queue, then select **Export all queued** to create all queued results and show one result list at the end. Use **Export selected** to create just one result: success removes that reservation and keeps the dialog and remaining queue open; failure or overwrite cancellation retains the reservation. Closing the dialog discards unexecuted reservations. Select a reservation to change its destination or file name and choose **Update reservation**, or move, remove, or clear it. The same destination cannot be queued twice, and the open original cannot be selected as output. Existing output files require an explicit overwrite confirmation. The application safely integrates current edits before output.

Hover over the destination folder field to see the complete path, or select the field and use Ctrl+A, Ctrl+C to copy it. TXT markup removal defaults to OFF, including Quick Note; previously saved settings remain in effect.

Use **Move up** and **Move down** to change the queue order. Both separate output and merging run from top to bottom. **Merge queued PDFs** is available only with at least two reservations, all of them annotated PDFs or selected-page PDFs. Choose one destination; the merged PDF preserves each reservation's page order, crop, scale, and annotation options. Individual reservation destinations are not written, and the results dialog lists only the merged file. Success clears the completed queue while keeping the dialog open. Destination or overwrite cancellation and output failure retain the queue. PNG or note reservations prevent merging.

After output, **Open with OS** in the results dialog reveals the selected output in Explorer. PDFs can open in the read-only viewer; other formats open with their associated application.

**Browse** opens the app's file picker, where you can navigate to a destination folder and enter a file name together. Selecting a listed file copies its name into the input. Choosing a destination does not export or overwrite anything. Quick PDF, Quick Note and other output-path prompts use the same picker. **OS Standard Dialog** switches to the Windows save dialog and retains the folder and entered name. When **Use Windows standard file/folder selection** is enabled in General settings, output destination selection opens the Windows save dialog directly. Canceling leaves the destination unchanged. The folder hierarchy display remains a separate browsing and operation feature.

The app's file and folder picker shows folders whose names include terms such as **course** or **lecture** in an upper **Suggestions** pane. When choosing a workspace, the current workspace is also suggested if it appears in the listing. Below a divider, **All entries (name order)** lists every folder followed by every file, including the suggestions. Either pane leads to the same location; selecting the same location in both panes for a multiple selection returns it once. Temporary path registration, opening files and choosing output destinations share this display. Locations without suggestions and the drive list use only the normal listing.

**Quick PDF** uses the annotated-PDF settings saved with **Save as Quick PDF** in the dialog. **Quick Note** always creates TXT; save its text, math, comment-line, and markup choices with **Save as Quick Note**. **View quick output settings** opens a compact, immediately closable summary of both current quick-output configurations. Output always creates a separate result and does not overwrite the open original.

### PDF annotation output

The main application exports standard PDF annotations: `Highlight`, `Ink`, `FreeText`, `Square`, `Circle`, and appearance-preserving `Stamp`. Text boxes prefer `FreeText`; the setting can request `Stamp`, and unsupported font appearances also use `Stamp`. This is not a flattened-output switch. See [Annotations in exported PDFs](Using_the_App.md#annotations-in-exported-pdfs) for the type mapping and editing behavior.

PNG output includes native PDF annotations, while **Include annotations** controls the separate app annotation layer.

## Memo

One plain-text memo per workspace holds search terms, source locations and working notes. Tools > Workspace memo and the search screen's Open memo button open the same editor. Closing search or settings leaves it open. Markdown and rich formatting are not supported.

Ctrl+S while typing and the Save button save only this memo. Closing it and exiting the application also save automatically. Failure cancels closing or switching and keeps the text. The original is `__pdf_note_workspace__/__memo__/workspace_memo.txt` inside the workspace (UTF-8, maximum 1 MiB). Settings presets do not include it.

Input is limited to 1 MiB of UTF-8 with LF line endings. Oversized input or pastes are rejected as a whole, without truncating or changing the previous text. Edited text is saved with LF endings; an unedited original retains its exact line endings and BOM.

You may edit the original directly. Conflicting external changes stop saving instead of being overwritten. Reload original first archives your draft in `backups/`, then reads the external version. Previous originals are also retained there. A durable `workspace_memo.recovery` draft is restored the next time you open the memo after an interrupted run. Do not delete this folder's data after a failure or crash. Old global/search memo files are not automatically converted or deleted; copy any needed contents into the new memo manually.

## PDF page rotation

Use **View → Rotate** to turn either the current page or all pages clockwise or counterclockwise by 90 degrees, or restore their original orientations. Use the all-pages commands for a PDF distributed sideways. They add the same turn to each page, preserving differences from individual page adjustments. Page rotations are saved in the PDF's last-view record. On reopening, rotation is restored before zoom and position; fit-width and fit-page use the rotated dimensions too. The source PDF orientation is unchanged. New text, math and shapes are upright on the current screen, then remain attached to the paper as it rotates. Their positions and content orientations are saved in `.clrop`.

Rotations use four clockwise quarter-turn states, stored in the workspace's `__pdf_note_workspace__/__tmp__/pdf_view_positions.txt`. Older records without rotations use zero additional rotation. Rotation data with a different page count is not applied. Read or write failures preserve the existing record and failed saves are reported.

The PDF export dialog's **Keep page rotation** checkbox starts ON. It captures each page's view angle when the export setting is added; queued and merged exports retain those angles. Turn it OFF to export in the original PDF orientation. Width and height refer to the selected output orientation. Quick PDF also defaults to ON. Change this checkbox and select **Set as Quick PDF** to save its ON/OFF preference. The angles themselves are not saved; Quick PDF uses the view angles at the time of export. PNG retains the original PDF orientation.

## PDF scrolling

Touchpad scrolling preserves fine input deltas. **Touchpad scroll stabilization** in Basic settings defaults to ON. In PDF and rendered note views it reduces small reversals during continuous scrolling. Sustained movement in the opposite direction takes effect. Turn it OFF to bypass this suppression. High-resolution wheels that send fine input deltas may also receive stabilization. The note's raw text editor uses standard Windows scrolling.

The annotation, lecture, session, PDF and note lists, and the application's file picker, accumulate fine scroll input separately for each list and move by whole rows, following the Windows wheel setting. Ordinary annotation-list updates retain your browsing position; an action that reveals a newly added annotation brings that item into view.

For PDF scrolling, arrow keys move by a fixed amount. With the PDF focused, hold `W` for up, `A` for left, `S` for down, or `D` for right to scroll smoothly. Hold two keys to move diagonally; releasing the keys stops movement. These PDF controls work with Vim controls on or off. While editing PDF text, the keys enter text.

## Input and formatting palette

Choose **Writing guide** to open the bundled [Note writing guide](Note_Writing_Guide.md) in the read-only viewer. It works without an open note and preserves the input field and note contents.

Use the lower-left input and formatting palette to insert text with **Apply** or Enter. Half-width and full-width spaces are preserved, and no trailing newline is added automatically. With the input field empty, formatting applies to the selected note text. With text in the input field, insertion occurs immediately after the selection. If both are empty, the note stays unchanged and a notice explains what to do.

Bold, italic and strike use Markdown `**`, `*` and `~~`; headings use `#` through `######`. Supported auxiliary attributes add text color, background, underline, size and other display properties. Headings, bullets and quotes inserted within a paragraph receive the necessary boundary newlines. Formatting generation requires a Markdown note. For `.txt`, `.csv` and `.tex`, formatting is declined and the input field is kept; clear the formatting controls to insert ordinary text.

While IME composition is active, Enter commits composition; apply after committing. The Apply button also waits for composition to finish. The input field clears only after insertion is verified, and stays intact when no note is open or formatting cannot be applied. Note undo and redo restore both insertion and selected-text formatting.

## Annotation shortcut overview and settings

Text markers follow columns in vertical PDF text and rows in horizontal PDF text. A vertical underline-style marker appears on the right of the column. Direction is inferred from character positions and spacing; different directions and substantially different sizes, such as ruby text, remain separate. Characters with insufficient direction evidence retain individual bounds. Use a free marker for lettering stored only in an image.

In the main application, **Text color** displays the original glyph shapes in the selected color. When text data is present in the PDF page, this also helps read text matching its background or covered by a shape. It does not restore lettering stored only in an image or text that has been deleted.

Choose **Translucent** or **Multiply** in the annotation panel for shape fills and outlines, text markers, underlines, and free markers. Shapes initially use Translucent; markers use Multiply. Multiply keeps dark text dark. The selected mode applies to subsequent creations. To change an existing annotation, use **Display mode** in its properties. Existing saved annotations retain their previous display. This display choice does not change PDF or PNG output.

Choose **Arc (parenthesis)** for an open curve. Drag its bounding rectangle to set the vertical span and bow depth. Drag right for `(` or left for `)`. Use the rotation handle after placement or **Alt+click** for further rotation. As with straight lines, choose solid or dashed style, width, color and opacity. Fill, black/white boundary and blend controls are hidden for this shape.

The shape **Line style** list offers **Solid**, **Dashed**, **Boundary (black)** and **Boundary (white)** in one control. Solid and Dashed use the shape color; black and white boundaries are solid. Filled shapes retain their fill color with the selected boundary drawn over it; outline-only shapes use a black or white outline. Boundaries use normal compositing with the shape's width and opacity. Choose the creation default in the annotation panel or change an existing shape in its properties. The boundary is also included in PDF and PNG output. Existing shapes retain their appearance.

After placing a shape, a temporary rotation handle appears, connected by a line to its fixed center pivot. Drag the handle to rotate the whole shape. Use **Alt+click** on an existing shape to show the same handle; release Alt before dragging if desired. Releasing the mouse commits one undoable rotation. Esc during a drag cancels that drag and hides the handle. Esc or another canvas click also hides the handle; the click continues its normal action. **Ctrl+drag** retains its move behavior. New rotated ellipses use the ordinary ellipse tool and this rotation operation; existing rotated-ellipse data retains its appearance.

Text color preserves filled, stroked, and filled-and-stroked lettering. Partially covered edge pixels may retain some of the original color.

Open **Settings → Annotation → Controls and shortcuts → Open shortcut overview and bindings** to review and edit bindings in one panel. Common fixed keys appear beside the editable list. Direct tool bindings are independent of annotation bar visibility, disabled state, and order, including hidden buttons. Image, read-only preview, and save-time editing restrictions remain in effect. Text entry takes priority.

New defaults use `Ctrl+1–9` for Select, Pan, Magnifier, Text, Marker, Free marker, Pen, Line, and Shape. `Ctrl+0` stays reserved for 100% zoom. Existing custom settings are retained. Use **Add unassigned Ctrl+1–9**, review the list, and save to add only unused combinations.

Choose a main key and the Ctrl, Shift, and Alt checkboxes, then choose a tool. Top-row digits and numpad keys are separate choices. The composed chord, duplicate or reserved-key conflicts, and any overridden common action appear immediately. **Press keys to assign** records an actual combination while suppressing its original action. Esc cancels recording only.

Update the list or select another row to retain the draft, then save. Cancel and failed saves preserve the saved bindings. Saving an empty list disables individual bindings. Earlier category, Ctrl+Alt+digit, and numpad bindings remain editable.

## Common action shortcuts

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

## Vim-style note controls

With Vim controls enabled, press `Esc` or `Ctrl+[` to enter Normal mode while keeping keyboard focus in the note. Finish IME composition first. Normal disables the note's IME; pressing `i` returns to Insert and restores IME input. Use `h` for left, `j` for down, `k` for up, and `l` for right; arrow keys move in the same directions. Pressing `h` at the start of the note keeps input in the note.

In Normal mode, `Esc` clears a selection or pending command and keeps Normal mode active. Press `i` to resume typing. To change panes, use `Ctrl+h` for the file list or `Ctrl+k` for the PDF.

From the PDF, `Ctrl+j` moves focus to the note at its current caret position. With Vim controls enabled, it enters Normal; press `i` to start typing. With Vim controls off, it uses standard input.

The note search field opened with `/` supports IME input. Closing it with `Esc` returns keyboard focus to the note in Normal mode.

## Operation and write checks

**Help > Checks** groups Operation and write checks, Asset presence check, PDF Information, and Note Information.

Open **Help > Checks > Operation and write checks** to see all 16 built-in checks and the last result and time for each current target. Results are **No record**, **Failed**, **Succeeded**, **Check itself failed**, or **Canceled**. A missing target or PDF conversion in Lite reports **Check itself failed**. **Details** explains the operation, failure stage, target and retained data locations.

**Current PDF read check** checks the PDF header, opening with PDFium and page presence on disk, without writing or rendering every page. **Current note read/write check** reads the entire note on disk, checks read/write access and tests a new file in the same directory for writing, reopening, comparison and deletion. It does not modify the original or unsaved edits. Missing files or directories report **Check itself failed**, with the cause in Details. In separate-folder mode, **Organize session files** creates pdf and note folders; checks do not create them.

Checks cover the current PDF and note directories and direct PDF and note placement in the session root separately. Rows with the same destination share the latest result. Checks do not change the file layout setting. Like **Asset presence check**, the buttons start with **Details** and **Refresh** and end with **Close**.

Select a row and choose **Run check**. Write checks create a dedicated new file, write and read it back, compare its contents and remove it. Existing settings and documents are preserved. PDF conversion uses a fixed DOCX demo in a dedicated temporary area and validates the generated and placed PDFs and byte equality. Shared LibreOffice cache writes are not a check item. Success applies to the shown time, target and operation; it does not guarantee future saves or conversion fidelity for every document.

Conversion data stays inside `__pdf_note_workspace__/__tmp__/lo` in the selected workspace. The conversion temporary-parent write check targets `__tmp__`. If it cannot be written, conversion stops without falling back to AppData or the Windows temporary folder. Writes outside the application folder are allowed for the selected workspace, folders registered through **Temporary Path**, and explicitly chosen save or export destinations. **Temporary Path** registers external folders; it does not select the conversion temporary area.

The limit is shared by all items. Five checks with unchanged results trigger a five-minute cooldown. First results, changed results and cancellations do not count. The remaining time is shown below the list. During cooldown, untested items and previous **Failed** results after confirmed corrective changes can use a shared allowance of two additional checks. The remaining allowance is shown. Closing the window keeps the limit; restarting the application resets it.

Actual normal-use writes of settings, temporary saves, recovery data, logs, PDFs and notes, and PDF conversion results also update the latest status. Details identifies normal use or a manual check and shows the actual operation target. Normal-use success covers that operation only; it does not establish completion of the manual check's reopen, comparison or deletion steps. Skipped saves and cancellations do not add successes, and normal-use updates do not affect the check count or cooldown.

Latest results are saved in the workspace's `__pdf_note_workspace__/__log__/write_checks.log`. Saving removes oldest results above 64 records or 64 KiB while preserving the newest entry. A removed target's result is shown as No record. If saving fails, results remain on screen with an unsaved notice. **Retry saving results** writes the unsaved results to the log again without repeating checks or counting toward the check limit. Opening or refreshing the window does not run checks. Refresh after changing targets before running another check.

## Settings

Open the unified settings dialog from **Settings > Settings Dialog...**. Choose pages such as Environment, Bottom pane, Color palette, Files, PDF, and Annotations from the list on the left. Open Asset presence check from **Help > Checks**.

Use **Save settings** or **Save and close** to save and apply edits. **Revert this page** restores the current page's unsaved inputs to their saved values. Color palette changes save automatically; check their save status on the palette page.

On settings pages with a section selector, use it to jump directly to a section. It follows the section currently shown while you scroll.

In **Settings > Bottom pane**, choose the default function for the bottom-right pane: continue note text, show headings, enter MathBox content, or use status assist. Save or Apply persists the selection and activates it now.

### Status assist

A subtle dedicated header distinguishes this pane from a note pane, and it scrolls vertically when needed. Items follow the order PDF → note display → fonts and background → input → analysis details. Settings appear beside their related state. Tinted rows can be clicked to change a setting; read-only or unavailable rows are not tinted. Source font identifies the font used for source display, not the note's text.

The page-number and zoom displays drawn on the PDF can be turned on or off independently. Enabling Vim controls immediately moves focus to the note's Normal mode. During IME composition, commit or cancel the composition before toggling Vim controls. Related controls appear only while Vim controls are enabled. The Normal-mode source caret-line preference is available only while rendering is on. Note click shows whether a click enters Insert or keeps Normal. Changes affect the display immediately without changing the note text; preferences are retained when their controls become unavailable. In Insert mode, the caret line and selected ranges are shown as source text. With rendering off, the entire note is source text.

Display setting identifies the chosen display mode. Current display reports the actual structured view, source editing ranges, or temporary source-view fallback; clicking this row does not change the setting. Operation target identifies the area receiving commands. Keyboard focus identifies the actual focused control. Input mode reports the note's Normal, Insert or Visual state, or search input. During Normal note navigation, both target and focus read Note. Moving to search, PDF or a file list updates the focus label. With Vim controls off, the input mode reads Standard input. Set the next-launch default from Settings.

Use **View > Bottom pane (temporary)** to switch the current display while retaining the saved default. Saving or applying the bottom-pane settings page later stores and activates the dialog selection, including reapplying the same saved value. Merely opening or closing the page, or saving only another page, preserves the temporary display. A failed save retains the saved default, temporary display and editable draft. Reloading content or starting again uses the saved default.

Changes saved or applied in the settings dialog are persistent settings. Items with a separate save mechanism, such as the palette, retain that mechanism.

### Asset presence check

Presence checks are ten seconds apart, with the remaining time shown on the refresh button. Closing and reopening retains the previous snapshot and cooldown; restarting resets them. There is no automatic recheck during cooldown. Confirmed absence is **Missing**; permission, drive or other inspection errors report **Check itself failed**, with the cause in Details. Unselected targets show **Not selected**.

Theme files that could not be inspected remain listed as failures, and the theme listing also reports failure. Content viewing distinguishes current absence, read errors and files exceeding the display limit. Read errors include the error cause; viewing does not modify files.

The header shows the last check time in local time, and the list shows changes since the preceding check of the same workspace: **Present → missing**, **Missing → present**, **Newly observed**, **Type changed**, or **No change**. The first check, the first check after a workspace switch, and checks after restarting show **No previous check**. If either inspection could not determine presence, the comparison is **Cannot compare**, rather than a claim of disappearance. Previously observed themes remain listed when missing and are checked again on later refreshes. Details shows the current and previous check times and states. Results and comparisons are held only for the current application run and are not saved to a file. If the overall inspection fails, the previous results and check time are retained.

**Details** in both Asset presence check and the operation and write checks shows status, paths, then additional information. Hover over an abbreviated path to see its full value, or use **Copy paths** to copy the complete paths.

Operation and write checks report unreadable records separately from unsaved results, with the cause in Details. A failed record read is not treated as no record. Unsaved results remain on screen; **Retry saving results** retries saving without running the check again.

Normal startup-setting reads, workspace listings and PDF opens also update results automatically. First results and changes are recorded immediately. Repeated identical reads update the display while record writes are grouped at ten-second intervals. Pending saves are shown and also saved with the next record write, workspace switch or exit. Page rendering does not repeatedly record results or start extra checks, and normal reads do not count toward the manual check limit.

Open **Help > Checks > Asset presence check** for a separate window with the same description, report list and action-button arrangement as the operation and write checks. Rows show item, presence, type and target name. Use **Details** or double-click a row to inspect its location; **Copy path** copies the complete path. Outside cooldown, presence is checked when a new window opens and when you choose **Refresh presence**, without changing settings or documents. Bringing an already open window to the front does not recheck presence, and there is no automatic monitoring. File content viewing remains available; **Open with OS** shows a selected folder in Explorer. Save or load presets through **Settings > Presets**.

In **Settings > Environment > File Lists > Lecture Order**, set priorities from 0 to 9999 for managed lectures and temporary paths. Right-click a lecture or temporary path in the directory list and choose **Change priority...** to open the editor with that entry selected. Lower numbers appear first; ties use name order, and unassigned entries appear last in name order.

New temporary paths use 5000, or the next available value (5001, 5002, and so on) when occupied. Existing values stay unchanged. Adjust priorities before adding a path when all values from 5000 to 9999 are occupied. Priority order combines managed lectures and temporary paths in the same numeric ordering. In name or recent-use order, temporary paths are ordered by priority ahead of the managed lectures.

The editor saves changes immediately. Managed lecture priorities use `lecturePriorities` in workspace settings and are included in settings presets. Temporary-path priorities use `tempExternalLecturePriorities` alongside the registered paths in the application's `pdf_note_workspace_setup.json`; they are shared between workspaces in that application.

Use **Check sizes** to inspect the management directory and its logs, temporary work, recovery data and backups in the background. Sizes, observed file counts and size changes are separate from presence checks. Original documents outside the management directory are excluded; no files are created, changed or deleted. Parent rows include child rows. Cancellation and inspection errors show partial results. See [Inspect log and generated-file sizes](Troubleshooting.md#inspect-log-and-generated-file-sizes) for scope and limits.
