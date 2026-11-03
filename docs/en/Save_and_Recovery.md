# Saving and Recovery

Edits are protected internally before they are written to an original file. Save Work, normal exit, and pre-output processing create a pre-save copy and update the original only after the safe write succeeds. Document/session switching keeps the protected work without starting original-file writes.

## Protecting open originals

Displayed PDFs, images, and notes are protected against deletion, moving, and renaming from Explorer and other applications, even after saving. Close the display or switch to another file before organizing an original externally. External editors that save by replacing the original are also restricted.

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

If protected edits cannot be applied to an original, review the final content in the recovery screen. You can also select the item in **Save > Review Save Status...** and choose **Recover**. Conflicts and unreadable data use the same screen. Saving a readable portion requires explicit confirmation when some content could not be recovered.

For PDFs, drag an annotation group from a failed file into the original group to append it. Existing annotations remain. Check the finished PDF in the center and the type/page lists; individual annotations are not edited here. If the target is wrong, choose a PDF and check the annotation overlay before saving. For notes, edit the final text while consulting read-only source versions.

The recovery manager stays on top and blocks ordinary application operations until you choose **Save and close** or **Defer and close**. The window close button offers save, defer, or continue. Deferring protects the candidate for resumption without updating the original. Failed saves or protection leave the screen open. **Save to a new file and close** can rescue work when the original destination is missing; existing files are never overwritten by this action.

Candidates and failed source data remain under `__pdf_note_workspace__/__escape__/recovery/`. Keep these files while investigating a problem. A failed new-file PDF/annotation save also retains its candidate and any files already created; retry with another new name.

The management folder is named `__pdf_note_workspace__`. Folders using the previous name are not automatically renamed, migrated, or read for compatibility.

Check for unintegrated changes before restoring. Use undo/redo for a recent edit; use Restore/Backup for a previously saved state. Do not remove or move `__pdf_note_workspace__` before investigating a problem.

Before a large edit or external sharing, integrate with `Ctrl+S` and then copy the complete workspace.
