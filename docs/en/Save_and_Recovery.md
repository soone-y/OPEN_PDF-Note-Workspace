# Saving and Recovery

Edits are protected internally before they are written to an original file. Save Work, normal exit, and pre-output processing create a pre-save copy and update the original only after the safe write succeeds. Document/session switching keeps the protected work without starting original-file writes.

## Choose the right action

- Continue editing: automatic work protection keeps the in-progress state.
- Commit a checkpoint: use `Ctrl+S` or `Save > Save Work`.
- Review work that could not be saved: open `Save > Review Save Status...`.
- Restore a PDF position or last-open time: select the item from `Restore`. Use `Restore > Backups` to restore or individually delete a saved backup. Select an item and choose **Details...** to review and copy its full restore-destination and metadata paths first.

## Important notes

Check for unintegrated changes before restoring. Use undo/redo for a recent edit; use Restore/Backup for a previously saved state. Do not remove or move `__resource__` before investigating a problem.

Before a large edit or external sharing, integrate with `Ctrl+S` and then copy the complete workspace.
