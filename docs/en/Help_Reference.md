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

## Settings

Open the unified settings dialog from **Settings > Settings Dialog...**. Its tabs cover General, Notes, Markup, Annotations, Color palette, and Settings & assets.

Use **Apply** or **OK** to save and apply edits. **Revert this tab** restores only the current tab's unapplied inputs to their saved values.

Use the section selector at the top of each settings tab to jump directly to a section. It automatically follows the section currently shown while you scroll.

In **General > Display and Interaction**, choose the function for the bottom-right pane: continue note text, show headings, enter MathBox content, or use status assist.

### Status assist

A subtle dedicated header distinguishes this pane from a note pane, and it scrolls vertically when needed. **Change settings** contains only settings that this pane can change, including zoom, the zoom display drawn on the PDF, annotation visibility, note display, raw-mode wrapping, and keyboard controls. Turning off the PDF zoom display keeps the page number visible. Enabling keyboard controls immediately moves focus to the note's normal mode. **Monitor** contains values to inspect: PDF/note state, character count, focus, IME, and selection. Set the next-launch default from Settings.

Use **View > Bottom-right pane function** to switch it immediately for the current run only. This choice is not saved; reloading content or starting again restores the default set in General settings.

### Settings & assets

This tab lists the file or folder type, item name, and present/missing status on one line, followed by its complete path. Thin separators distinguish entries. Use the horizontal scroll bar for long paths, and either double-click a path line or choose **Copy path** to copy it. **Refresh presence** only checks whether items exist; it does not modify settings or documents. To inspect contents read-only, select an item and choose **View file contents** or **View folder contents**. You can also manage global memos and save or load presets from this page.
