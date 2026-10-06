# Help Reference

## About this application

This is a local application for PDFs, notes, and annotations. It has no external communication features. While you edit, it does not write directly to the original PDF; it uses recoverable working data.

## Basic flow

1. Select a workspace in a writable location.
2. Open and edit a PDF or note.
3. Save and integrate at a suitable stopping point.

## Saving and recovery

Changes are protected internally while you edit. Save Work, normal exit, and pre-output processing create a backup before safely saving to the original file. Switching keeps protected work without writing the original. If a save fails, do not alter the PDF, note, or `__resource__`; read [Saving and Recovery](Save_and_Recovery.md) and [Troubleshooting](Troubleshooting.md).

## Notes and annotations

- `.clro` and `.md` are text formats for notes.
- `.txt` is plain text without formatting.
- `.clrop` is annotation data associated with a PDF. Do not edit or rename it by hand.

See [File Formats](File_Formats.md) for details.

## Full and Lite editions

The Full edition can convert Office files to PDF locally with bundled features. Lite does not include the conversion runtime. Neither edition uses Microsoft Office or an online conversion service.

## Read-only viewer

`readonly_viewer.exe` is a viewer for PDFs and bundled documents. Use `pdf_note_workspace.exe` to edit.

## Output

Use **Output > Output Dialog...** to choose annotated PDF, selected-page PDF, single-page PNG, TXT, Markdown, or HTML. Set the destination folder and output file name before output; useful defaults are supplied and may be changed. With no queue, **Export** creates the current output alone. Add each desired setting to the queue, then select **Export** to create all queued results; select a reservation and use **Export selected** to create just that result. Select a reservation to change its destination or file name and choose **Update reservation**, or move, remove, or clear it. The same destination cannot be queued twice, and the open original cannot be selected as output. Existing output files require an explicit overwrite confirmation. The application safely integrates current edits before output.

After output, the results dialog can reveal any output in Explorer. PDFs can open in the read-only viewer; other formats open with their associated application.

**Quick PDF** uses the annotated-PDF settings saved with **Save as Quick PDF** in the dialog. **Quick Note** always creates TXT; save its text, math, comment-line, and markup choices with **Save as Quick Note**. **View quick output settings** opens a compact, immediately closable summary of both current quick-output configurations. Output always creates a separate result and does not overwrite the open original.

## Memo

One plain-text memo per workspace holds search terms, source locations and working notes. Tools > Workspace memo, the search screen's Open memo button and the Memo button in Settings & assets open the same editor. Closing search or settings leaves it open. Markdown and rich formatting are not supported.

Ctrl+S while typing and the Save button save only this memo. Closing it and exiting the application also save automatically. Failure cancels closing or switching and keeps the text. The original is `__resource__/__memo__/workspace_memo.txt` inside the workspace (UTF-8, maximum 1 MiB). Settings presets do not include it.

Input is limited to 1 MiB of UTF-8 with LF line endings. Oversized input or pastes are rejected as a whole, without truncating or changing the previous text. Edited text is saved with LF endings; an unedited original retains its exact line endings and BOM.

You may edit the original directly. Conflicting external changes stop saving instead of being overwritten. Reload original first archives your draft in `backups/`, then reads the external version. Previous originals are also retained there. A durable `workspace_memo.recovery` draft is restored the next time you open the memo after an interrupted run. Do not delete this folder's data after a failure or crash. Old global/search memo files are not automatically converted or deleted; copy any needed contents into the new memo manually.

## PDF scrolling

For PDF scrolling, arrow keys move by a fixed amount. With the PDF focused, hold `W` for up, `A` for left, `S` for down, or `D` for right to scroll smoothly. Hold two keys to move diagonally; releasing the keys stops movement. These PDF controls work with Vim controls on or off. While editing PDF text, the keys enter text.

## Vim-style note controls

With Vim controls enabled, press `Esc` or `Ctrl+[` to enter Normal mode while keeping keyboard focus in the note. Finish IME composition first. Normal disables the note's IME; pressing `i` returns to Insert and restores IME input. Use `h` for left, `j` for down, `k` for up, and `l` for right; arrow keys move in the same directions. Pressing `h` at the start of the note keeps input in the note.

In Normal mode, `Esc` clears a selection or pending command and keeps Normal mode active. Press `i` to resume typing. To change panes, use `Ctrl+h` for the file list or `Ctrl+k` for the PDF.

From the PDF, `Ctrl+j` moves focus to the note at its current caret position. With Vim controls enabled, it enters Normal; press `i` to start typing. With Vim controls off, it uses standard input.

The note search field opened with `/` supports IME input. Closing it with `Esc` returns keyboard focus to the note in Normal mode.

## Settings

Open the unified settings dialog from **Settings > Settings Dialog...**. Its tabs cover General, Notes, Markup, Annotations, Color palette, and Settings & assets.

Use **Apply** or **OK** to save and apply edits. **Revert this tab** restores only the current tab's unapplied inputs to their saved values.

Use the section selector at the top of each settings tab to jump directly to a section. It automatically follows the section currently shown while you scroll.

In **General > Display and Interaction**, choose the function for the bottom-right pane: continue note text, show headings, enter MathBox content, or use status assist.

### Status assist

A subtle dedicated header distinguishes this pane from a note pane, and it scrolls vertically when needed. Items follow the order PDF → note display → fonts and background → input → analysis details. Settings appear beside their related state. Tinted rows can be clicked to change a setting; read-only or unavailable rows are not tinted. Source font identifies the font used for source display, not the note's text.

The page-number and zoom displays drawn on the PDF can be turned on or off independently. Enabling Vim controls immediately moves focus to the note's Normal mode. During IME composition, commit or cancel the composition before toggling Vim controls. Related controls appear only while Vim controls are enabled. The Normal-mode source caret-line preference is available only while rendering is on. Note click shows whether a click enters Insert or keeps Normal. Changes affect the display immediately without changing the note text; preferences are retained when their controls become unavailable. In Insert mode, the caret line and selected ranges are shown as source text. With rendering off, the entire note is source text.

Display setting identifies the chosen display mode. Current display reports the actual structured view, source editing ranges, or temporary source-view fallback; clicking this row does not change the setting. Operation target identifies the area receiving commands. Keyboard focus identifies the actual focused control. Input mode reports the note's Normal, Insert or Visual state, or search input. During Normal note navigation, both target and focus read Note. Moving to search, PDF or a file list updates the focus label. With Vim controls off, the input mode reads Standard input. Set the next-launch default from Settings.

Use **View > Bottom-right pane function** to switch it immediately for the current run only. This choice is not saved; reloading content or starting again restores the default set in General settings.

### Settings & assets

This tab lists the file or folder type, item name, and present/missing status on one line, followed by its complete path. Thin separators distinguish entries. Use the horizontal scroll bar for long paths, and either double-click a path line or choose **Copy path** to copy it. **Refresh presence** only checks whether items exist; it does not modify settings or documents. To inspect contents read-only, select an item and choose **View file contents** or **View folder contents**. You can also edit the memo and save or load presets from this page. Memo original, recovery and backup entries show filenames only; use Copy path to obtain their complete locations.
