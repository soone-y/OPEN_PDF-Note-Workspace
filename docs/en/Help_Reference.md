# Help Reference

## About this application

This is a local application for PDFs, notes, and annotations. It has no external communication features. While you edit, it does not write directly to the original PDF; it uses recoverable working data.

## Basic flow

1. Select a workspace in a writable location.
2. Open and edit a PDF or note.
3. Save and integrate at a suitable stopping point.

## Saving and recovery

Changes are protected internally while you edit. Save Work, normal exit, and pre-output processing create a backup before safely saving to the original file. Switching keeps protected work without writing the original. If a save fails, do not alter the PDF, note, or `__pdf_note_workspace__`; read [Saving and Recovery](Save_and_Recovery.md) and [Troubleshooting](Troubleshooting.md).

## Notes and annotations

- `.clro` and `.md` are text formats for notes.
- `.txt` is plain text without formatting.
- `.clrop` is annotation data associated with a PDF. Do not edit or rename it by hand.

See [File Formats](File_Formats.md) for details.

## Full and Lite editions

The Full edition can convert Office files to PDF locally with bundled features. Lite does not include the conversion runtime. Neither edition uses Microsoft Office or an online conversion service.

## Read-only viewer

`readonly_viewer.exe` is a viewer for PDFs and bundled documents. Use `pdf_note_workspace.exe` to edit.

## File names and locations

**Edit > Rename** shows the full file name, including its extension. PDF names must keep `.pdf` (case-insensitive). Note names may use `.txt`, `.csv`, `.md`, `.markdown`, `.note`, `.tex`, `.icr` or `.clro`; renaming does not convert the contents. An invalid name or extension produces a notice and keeps the same input dialog open without renaming the file.

In the app's folder picker, long paths omit intermediate folders while retaining the destination folder. Hover over the path bar to see the complete path or use **Copy path** to copy it. Clicking a displayed path component still opens the corresponding folder.

## Output

Use **Output > Output Dialog...** to choose annotated PDF, selected-page PDF, single-page PNG, TXT, Markdown, or HTML. Set the destination folder and output file name before output; useful defaults are supplied and may be changed. With no queue, **Export** creates the current output alone. Add each desired setting to the queue, then select **Export** to create all queued results; select a reservation and use **Export selected** to create just that result. Select a reservation to change its destination or file name and choose **Update reservation**, or move, remove, or clear it. The same destination cannot be queued twice, and the open original cannot be selected as output. Existing output files require an explicit overwrite confirmation. The application safely integrates current edits before output.

After output, **Open with OS** in the results dialog reveals the selected output in Explorer. PDFs can open in the read-only viewer; other formats open with their associated application.

**Browse** opens the app's file picker, where you can navigate to a destination folder and enter a file name together. Selecting a listed file copies its name into the input. Choosing a destination does not export or overwrite anything. Quick PDF, Quick Note and other output-path prompts use the same picker. **OS Standard Dialog** switches to the Windows save dialog and retains the folder and entered name. When **Use Windows standard file/folder selection** is enabled in General settings, output destination selection opens the Windows save dialog directly. Canceling leaves the destination unchanged. The folder hierarchy display remains a separate browsing and operation feature.

The app's file and folder picker shows folders whose names include terms such as **course** or **lecture** in an upper **Suggestions** pane. When choosing a workspace, the current workspace is also suggested if it appears in the listing. Below a divider, **All entries (name order)** lists every folder followed by every file, including the suggestions. Either pane leads to the same location; selecting the same location in both panes for a multiple selection returns it once. Temporary path registration, opening files and choosing output destinations share this display. Locations without suggestions and the drive list use only the normal listing.

**Quick PDF** uses the annotated-PDF settings saved with **Save as Quick PDF** in the dialog. **Quick Note** always creates TXT; save its text, math, comment-line, and markup choices with **Save as Quick Note**. **View quick output settings** opens a compact, immediately closable summary of both current quick-output configurations. Output always creates a separate result and does not overwrite the open original.

### PDF annotation output

The main application exports standard PDF annotations: `Highlight`, `Ink`, `FreeText`, `Square`, `Circle`, and appearance-preserving `Stamp`. Text boxes prefer `FreeText`; the setting can request `Stamp`, and unsupported font appearances also use `Stamp`. This is not a flattened-output switch. Text recoloring remains unsupported. See [Annotations in exported PDFs](Using_the_App.md#annotations-in-exported-pdfs) for the type mapping and editing limits.

PDFs containing native annotations can be exported at 1x only; unsupported scaling stops without replacing the output. PNG output includes native PDF annotations, while **Include annotations** controls the separate app annotation layer.

## Memo

One plain-text memo per workspace holds search terms, source locations and working notes. Tools > Workspace memo and the search screen's Open memo button open the same editor. Closing search or settings leaves it open. Markdown and rich formatting are not supported.

Ctrl+S while typing and the Save button save only this memo. Closing it and exiting the application also save automatically. Failure cancels closing or switching and keeps the text. The original is `__pdf_note_workspace__/__memo__/workspace_memo.txt` inside the workspace (UTF-8, maximum 1 MiB). Settings presets do not include it.

Input is limited to 1 MiB of UTF-8 with LF line endings. Oversized input or pastes are rejected as a whole, without truncating or changing the previous text. Edited text is saved with LF endings; an unedited original retains its exact line endings and BOM.

You may edit the original directly. Conflicting external changes stop saving instead of being overwritten. Reload original first archives your draft in `backups/`, then reads the external version. Previous originals are also retained there. A durable `workspace_memo.recovery` draft is restored the next time you open the memo after an interrupted run. Do not delete this folder's data after a failure or crash. Old global/search memo files are not automatically converted or deleted; copy any needed contents into the new memo manually.

## PDF scrolling

For PDF scrolling, arrow keys move by a fixed amount. With the PDF focused, hold `W` for up, `A` for left, `S` for down, or `D` for right to scroll smoothly. Hold two keys to move diagonally; releasing the keys stops movement. These PDF controls work with Vim controls on or off. While editing PDF text, the keys enter text.

## Input and formatting palette

Use the lower-left input and formatting palette to insert text with **Apply** or Enter. Half-width and full-width spaces are preserved, and no trailing newline is added automatically. With the input field empty, formatting applies to the selected note text. With text in the input field, insertion occurs immediately after the selection. If both are empty, the note stays unchanged and a notice explains what to do.

Bold, italic and strike use Markdown `**`, `*` and `~~`; headings use `#` through `######`. Supported auxiliary attributes add text color, background, underline, size and other display properties. Headings, bullets and quotes inserted within a paragraph receive the necessary boundary newlines. Formatting generation requires a Markdown note. For `.txt`, `.csv` and `.tex`, formatting is declined and the input field is kept; clear the formatting controls to insert ordinary text.

While IME composition is active, Enter commits composition; apply after committing. The Apply button also waits for composition to finish. The input field clears only after insertion is verified, and stays intact when no note is open or formatting cannot be applied. Note undo and redo restore both insertion and selected-text formatting.

## Vim-style note controls

With Vim controls enabled, press `Esc` or `Ctrl+[` to enter Normal mode while keeping keyboard focus in the note. Finish IME composition first. Normal disables the note's IME; pressing `i` returns to Insert and restores IME input. Use `h` for left, `j` for down, `k` for up, and `l` for right; arrow keys move in the same directions. Pressing `h` at the start of the note keeps input in the note.

In Normal mode, `Esc` clears a selection or pending command and keeps Normal mode active. Press `i` to resume typing. To change panes, use `Ctrl+h` for the file list or `Ctrl+k` for the PDF.

From the PDF, `Ctrl+j` moves focus to the note at its current caret position. With Vim controls enabled, it enters Normal; press `i` to start typing. With Vim controls off, it uses standard input.

The note search field opened with `/` supports IME input. Closing it with `Esc` returns keyboard focus to the note in Normal mode.

## Operation and write checks

Open **Help > Operation and write checks** to see all 16 built-in checks and the last result and time for each current target. Results are **No record**, **Failed**, **Succeeded**, **Check itself failed**, or **Canceled**. A missing target or PDF conversion in Lite reports **Check itself failed**. **Details** explains the operation, failure stage, target and retained data locations.

**Current PDF read check** checks the PDF header, opening with PDFium and page presence on disk, without writing or rendering every page. **Current note read/write check** reads the entire note on disk, checks read/write access and tests a new file in the same directory for writing, reopening, comparison and deletion. It does not modify the original or unsaved edits. Missing files or directories report **Check itself failed**, with the cause in Details. In separate-folder mode, **Organize session files** creates pdf and note folders; checks do not create them.

Checks cover the current PDF and note directories and direct PDF and note placement in the session root separately. Rows with the same destination share the latest result. Checks do not change the file layout setting. Like **Asset presence check**, the buttons start with **Details** and **Refresh** and end with **Close**.

Select a row and choose **Run check**. Write checks create a dedicated new file, write and read it back, compare its contents and remove it. Existing settings and documents are preserved. PDF conversion uses a fixed DOCX demo in a dedicated temporary area and validates the generated and placed PDFs and byte equality. Shared LibreOffice cache writes are not a check item. Success applies to the shown time, target and operation; it does not guarantee future saves or conversion fidelity for every document.

Conversion data stays inside `__pdf_note_workspace__/__tmp__/lo` in the selected workspace. The conversion temporary-parent write check targets `__tmp__`. If it cannot be written, conversion stops without falling back to AppData or the Windows temporary folder. Writes outside the application folder are allowed for the selected workspace, folders registered through **Temporary Path**, and explicitly chosen save or export destinations. **Temporary Path** registers external folders; it does not select the conversion temporary area.

The limit is shared by all items. Five checks with unchanged results trigger a five-minute cooldown. First results, changed results and cancellations do not count. The remaining time is shown below the list. During cooldown, untested items and previous **Failed** results after confirmed corrective changes can use a shared allowance of two additional checks. The remaining allowance is shown. Closing the window keeps the limit; restarting the application resets it.

Actual normal-use writes of settings, temporary saves, recovery data, logs, PDFs and notes, and PDF conversion results also update the latest status. Details identifies normal use or a manual check and shows the actual operation target. Normal-use success covers that operation only; it does not establish completion of the manual check's reopen, comparison or deletion steps. Skipped saves and cancellations do not add successes, and normal-use updates do not affect the check count or cooldown.

Latest results are saved in the workspace's `__pdf_note_workspace__/__log__/write_checks.log`. Saving removes oldest results above 64 records or 64 KiB while preserving the newest entry. A removed target's result is shown as No record. If saving fails, results remain on screen with an unsaved notice. **Retry saving results** writes the unsaved results to the log again without repeating checks or counting toward the check limit. Opening or refreshing the window does not run checks. Refresh after changing targets before running another check.

## Settings

Open the unified settings dialog from **Settings > Settings Dialog...**. Its tabs cover General, Notes, Markup, Annotations, and Color palette. Open Asset presence check from Help.

Use **Apply** or **OK** to save and apply edits. **Revert this tab** restores only the current tab's unapplied inputs to their saved values.

Use the section selector at the top of each settings tab to jump directly to a section. It automatically follows the section currently shown while you scroll.

In **General > Display and Interaction**, choose the function for the bottom-right pane: continue note text, show headings, enter MathBox content, or use status assist.

### Status assist

A subtle dedicated header distinguishes this pane from a note pane, and it scrolls vertically when needed. Items follow the order PDF → note display → fonts and background → input → analysis details. Settings appear beside their related state. Tinted rows can be clicked to change a setting; read-only or unavailable rows are not tinted. Source font identifies the font used for source display, not the note's text.

The page-number and zoom displays drawn on the PDF can be turned on or off independently. Enabling Vim controls immediately moves focus to the note's Normal mode. During IME composition, commit or cancel the composition before toggling Vim controls. Related controls appear only while Vim controls are enabled. The Normal-mode source caret-line preference is available only while rendering is on. Note click shows whether a click enters Insert or keeps Normal. Changes affect the display immediately without changing the note text; preferences are retained when their controls become unavailable. In Insert mode, the caret line and selected ranges are shown as source text. With rendering off, the entire note is source text.

Display setting identifies the chosen display mode. Current display reports the actual structured view, source editing ranges, or temporary source-view fallback; clicking this row does not change the setting. Operation target identifies the area receiving commands. Keyboard focus identifies the actual focused control. Input mode reports the note's Normal, Insert or Visual state, or search input. During Normal note navigation, both target and focus read Note. Moving to search, PDF or a file list updates the focus label. With Vim controls off, the input mode reads Standard input. Set the next-launch default from Settings.

Use **View > Bottom-right pane function** to switch it immediately for the current run only. This choice is not saved; reloading content or starting again restores the default set in General settings.

### Asset presence check

Presence checks are ten seconds apart, with the remaining time shown on the refresh button. Closing and reopening retains the previous snapshot and cooldown; restarting resets them. There is no automatic recheck during cooldown. Confirmed absence is **Missing**; permission, drive or other inspection errors report **Check itself failed**, with the cause in Details. Unselected targets show **Not selected**.

Theme files that could not be inspected remain listed as failures, and the theme listing also reports failure. Content viewing distinguishes current absence, read errors and files exceeding the display limit. Read errors include the error cause; viewing does not modify files.

The header shows the last check time in local time, and the list shows changes since the preceding check of the same workspace: **Present → missing**, **Missing → present**, **Newly observed**, **Type changed**, or **No change**. The first check, the first check after a workspace switch, and checks after restarting show **No previous check**. If either inspection could not determine presence, the comparison is **Cannot compare**, rather than a claim of disappearance. Previously observed themes remain listed when missing and are checked again on later refreshes. Details shows the current and previous check times and states. Results and comparisons are held only for the current application run and are not saved to a file. If the overall inspection fails, the previous results and check time are retained.

**Details** in both Asset presence check and the operation and write checks shows status, paths, then additional information. Hover over an abbreviated path to see its full value, or use **Copy paths** to copy the complete paths.

Operation and write checks report unreadable records separately from unsaved results, with the cause in Details. A failed record read is not treated as no record. Unsaved results remain on screen; **Retry saving results** retries saving without running the check again.

Normal startup-setting reads, workspace listings and PDF opens also update results automatically. First results and changes are recorded immediately. Repeated identical reads update the display while record writes are grouped at ten-second intervals. Pending saves are shown and also saved with the next record write, workspace switch or exit. Page rendering does not repeatedly record results or start extra checks, and normal reads do not count toward the manual check limit.

Open **Help > Asset presence check** for a separate window with the same description, report list and action-button arrangement as the operation and write checks. Rows show item, presence, type and target name. Use **Details** or double-click a row to inspect its location; **Copy path** copies the complete path. Outside cooldown, presence is checked when a new window opens and when you choose **Refresh presence**, without changing settings or documents. Bringing an already open window to the front does not recheck presence, and there is no automatic monitoring. File content viewing remains available; **Open with OS** shows a selected folder in Explorer. Save or load presets through **Settings > Presets**.

Use **Check sizes** to inspect the management directory and its logs, temporary work, recovery data and backups in the background. Sizes, observed file counts and size changes are separate from presence checks. Original documents outside the management directory are excluded; no files are created, changed or deleted. Parent rows include child rows. Cancellation and inspection errors show partial results. See [Inspect log and generated-file sizes](Troubleshooting.md#inspect-log-and-generated-file-sizes) for scope and limits.
