# Saving and Recovery

Edits are protected in stage data before they are written to an original file. When you integrate with `Ctrl+S` or the Save menu, the application creates a backup and updates the original only after the safe write succeeds.

## Choose the right action

- Continue editing: automatic stage protection keeps the in-progress state.
- Commit a checkpoint: integrate with `Ctrl+S`.
- Review unintegrated changes: open Diff Manager from Operations or Save.
- Return to an earlier saved state: open `Save > Restore/Backup...`.

## Important notes

Check for unintegrated changes before restoring. Use undo/redo for a recent edit; use Restore/Backup for a previously saved state. Do not remove or move `__resource__` before investigating a problem.

Before a large edit or external sharing, integrate with `Ctrl+S` and then copy the complete workspace.
