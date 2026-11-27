# Saving and Recovery

Edits are protected internally before they are written to an original file. Save Work, normal exit, and pre-output processing create a pre-save copy and update the original only after the safe write succeeds. Document/session switching keeps the protected work without starting original-file writes.

## Protecting open originals

Displayed PDFs, images, and notes are protected against deletion, moving, and renaming from Explorer and other applications, even after saving. External editors that save by replacing the original are also restricted.

While the application is running, a PDF with staged annotation edits, or a note with staged edits, remains protected after its display closes or switches to another file. Checkpoints from earlier sessions also protect their original PDFs and notes when loaded. Before organizing a PDF or note externally, use **Save Work** to integrate its edits, then close its display. Explicitly discarding edits also releases protection once all checkpoints for that file are removed and its display is closed. Closing a display alone does not integrate edits. This deletion protection ends when the application exits.

If integration fails on exit, the application leaves the original files unchanged, shows a notice, and lets you choose **Hold**, **Recover**, or **Exit**. **Hold** keeps the application open. **Recover** opens the recovery window for the changes that could not be saved. **Exit** closes the application while keeping the unintegrated changes in stage, so you can integrate or recover them at next startup. The application does not exit if saving to stage also fails.

Other conditions do not trap the application either. If a settings, output, or similar window prevents exit, that window is brought forward with a notice. If the workspace memo cannot be saved, choose **Hold** or **Copy and Exit**, which copies the memo text to the clipboard before exiting. If integration runs for more than 30 seconds, choose **Wait** or **Cancel Exit**; the application never exits in the middle of a write. If the record of which files to reopen cannot be saved, exit continues, and the previous files may not reopen automatically next time.

When a conflicting external request to a protected PDF is detected, brief guidance appears near the lower-right corner. It recommends **Save Work** for edits not yet applied to the original, or closing the display when that is the only protection remaining. The notice is silent, does not take focus or intercept clicks, and disappears automatically. Repeated requests are generally grouped for 30 seconds, and ordinary warnings take priority. The notice does not identify a particular operation or user intent. Deletion protection remains in force when advisory observation is unavailable.

The application's own atomic saves and moves briefly release and reacquire protection for their target. External operations during that interval cannot be completely prevented. If reacquisition fails, the status shows "Protection pending" and recovery continues automatically without reopening the file. Do not delete or move the original externally until that status clears.

A completed move updates references to its destination independently of protection recovery. If protection fails after a save has replaced the original, the error distinguishes the completed replacement from pending protection. The save remains incomplete and protected work and recovery data are retained.

`File > Close Files` closes both the PDF and the note. `View > Close > PDF / Note` closes only the selected display. Both routes protect edits internally before closing and keep the selected lecture and session. If that protection fails, the displays stay open.

Closing a note ends that editor's undo/redo history. Unsaved edits remain in the internal recovery data and are restored when the note is reopened.

## Choose the right action

- Continue editing: automatic work protection keeps the in-progress state.
- Commit a checkpoint: use `Ctrl+S` or `Save > Save Work`.
- Review work that could not be saved: open `Save > Review Save Status...`.
- Restore a PDF position or last-open time: select the item from `Settings > Restore`. Use `Save > Saved file backups` to restore or individually delete a saved backup. Select an item and choose **Details...** to review and copy its full restore-destination and metadata paths first.

## Retry unsaved settings

Use **Save > Content not saved…** to review failed temporary-path registrations, theme saves, and palette saves. Requests rejected by the read/write check before a temporary path is added are retained too. After checking destination permissions or security controls, choose **Retry** to check access again and complete the registration or save. Existing registrations and the settings file remain unchanged when the access check fails.

The window shows the failure reason and intended content, with commands to copy the content or target path. If the target or settings have changed, the older request stops before overwriting them. Retry copies remain in memory for the current application session; exiting with pending requests displays a confirmation.

Failure notices also point to **Save > Content not saved…**. Temporary-path additions can also be retried from **File > Temporary Paths > Retry**. A single retryable request runs directly; when several requests are pending, select the target in the window.

Temporary-path additions and retries check folder access sequentially in the background. You can continue using the current PDF, note, and annotation panel while a check runs. One addition request can run at a time. The batch is registered after checks finish and the registered paths and settings still match their starting state. Changing the workspace or registrations prevents an older result from being applied. Closing the application cancels the addition before normal shutdown continues.

## Important notes

If protected edits cannot be applied to an original, review the final content in the recovery tool, which runs separately from the application. Choosing **Recover** exits the application with the unintegrated changes kept in stage and opens the recovery tool. You can also select the item in **Save > Review Save Status...** and choose **Recover**.

Even when the application cannot start, you can open the recovery tool directly by running `pdf_note_workspace.exe --recovery` in the application folder and choosing the workspace. It lists every stage in the workspace, including those whose original is not found. The application cannot start while the recovery tool runs, and the recovery tool asks you to exit the application while it runs. Conflicts and unreadable data use the same screen. Saving a readable portion requires explicit confirmation when some content could not be recovered.

For PDFs, drag an annotation group from a failed file into the original group to append it. Existing annotations remain. Check the finished PDF in the center and the type/page lists; individual annotations are not edited here. If the target is wrong, choose a PDF and check the annotation overlay before saving. For notes, edit the final text while consulting read-only source versions.

The review screen returns to the recovery tool list when you choose **Save and close** or **Defer and close**. The window close button offers save, defer, or continue. Deferring protects the candidate for resumption without updating the original. Failed saves or protection leave the screen open. **Save to a new file and close** can rescue work when the original destination is missing; existing files are never overwritten by this action.

Candidates and failed source data remain under `__pdf_note_workspace__/__recovery__/`. Keep these files while investigating a problem. A failed new-file PDF/annotation save also retains its candidate and any files already created; retry with another new name.

## Recover manually with Explorer

This procedure extracts unintegrated edits (stage) with Explorer and a text editor, without the application. If the application starts, use the recovery tool first (Recover in the application, or `pdf_note_workspace.exe --recovery`).

Before you start:
1. Exit the application and the recovery tool.
2. Copy the entire workspace folder to another location as a backup. Do not touch that copy.
3. Before replacing an original (note, PDF, or .clrop), make a copy of it.

Location (`__pdf_note_workspace__\__unsaved__\` directly under the workspace):
- `note\` Unintegrated note content. `note_<id>_r<number>.stage` is the text, in the same encoding and line endings as the original note. In the file with the same name plus `.meta.txt`, `target=` is the original note and `rev=` is the revision.
- `clrop\` Unintegrated PDF annotations. `clrop_<id>_r<number>.clrop.stage` is annotation data in the same format as .clrop. In its `.meta.txt`, `target=` is the original PDF and `dest=` is the .clrop to write.
- `clrop_journal\` Annotations added after the last stage. These cannot be integrated manually.
- Pre-save backups are in `__pdf_note_workspace__\__backup__\`; recovery tool candidates are in `__pdf_note_workspace__\__recovery__\`. See [Management folder](Help_Reference.md#management-folder-__pdf_note_workspace__) and `__pdf_note_workspace__/DELETE_OR_KEEP.txt` for which folders may be deleted.

Restore a note:
1. Open the `.meta.txt` in `note\` with Notepad and check the target in `target=`. If several share a target, the highest `rev=` is the newest.
2. Open the matching `.stage` in Notepad or a similar editor and check the content.
3. Copy the `.stage`, rename it to the file name in `target=` (including the extension), and place it at the original note's location. If the original note still exists, copy it first, then replace it.

Restore PDF annotations:
1. In the `.meta.txt` in `clrop\`, check `target=` (PDF) and `dest=` (.clrop). The highest `rev=` is the newest.
2. If the .clrop in `dest=` still exists, copy it first.
3. Copy the `.clrop.stage`, rename it to the file name in `dest=` (the PDF name with the .clrop extension), and place it in the PDF's folder.
4. If `clrop_journal\` has files with the same id, those annotations cannot be restored manually. Integrate them with the application or the recovery tool.

Clean up:
Keep the contents of `__unsaved__` until you have opened and checked the restored content in the application. Afterwards, discard the stage from Diff Manager in the application.

The management folder is named `__pdf_note_workspace__`. Folders using the previous name are not automatically renamed, migrated, or read for compatibility.

Check for unintegrated changes before restoring. Use undo/redo for a recent edit; use Restore/Backup for a previously saved state. Do not remove or move `__pdf_note_workspace__` before investigating a problem.

Before a large edit or external sharing, integrate with `Ctrl+S` and then copy the complete workspace.
