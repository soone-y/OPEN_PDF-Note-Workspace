# Troubleshooting

## First steps

If saving, loading, or display fails, do not delete, move, or overwrite the affected PDF, note, or `__pdf_note_workspace__` directory. Record the error message, file name, and time of the operation.

## Cannot save

- Confirm that the destination is writable.
- Check whether another application has the same PDF or note open.
- Keep `__pdf_note_workspace__`, copy the entire workspace to another writable location, and check there.

See [Saving and Recovery](Save_and_Recovery.md) for protected work and recovery.

## Cannot open or display a file

Check the intended use of the file type in [File Formats](File_Formats.md). Do not rename extensions or alter original and working files by hand.

## Office conversion and long paths

Office conversion supports long local file paths without a blanket 220-character cutoff. The file name component, Windows process working directory, and complete command line have separate limits. If an error identifies the runtime working directory, move the entire application to a shorter location. Keep existing workspace data intact. Conversion temporary files stay in the selected workspace's `__pdf_note_workspace__/__tmp__/lo/`; an unwritable location does not trigger a fallback to the OS temporary directory.

## Diagnostic log size

Diagnostic logs are for investigation and are stored in the workspace's `__pdf_note_workspace__/__log__/` directory. The preview, switching-time, crash, startup-monitor, and Office-conversion logs share a 10 MB (10,000,000-byte) limit. Space for a short stop record is reserved; an append that would exceed the remaining budget stops output. Existing logs are never truncated, and restarting the app preserves the stop. Editing and saving continue.

The debug-log section of general settings shows the size and stop state at the time of inspection. To resume, preserve any diagnostic logs you need, then explicitly use the developer menu's log-deletion command. Creating a ZIP alone does not reduce the original logs. ZIP copies and the separately managed `write_checks.log` are outside this budget. Log-setting changes, including Office conversion, apply on the next launch.

The `__tmp__` and `__escape__` directories can contain unsaved and recovery data. Do not delete the entire management directory to clean up logs.

## Inspect log and generated-file sizes

Use Help > Asset presence check > Check sizes to inspect the selected workspace's management directory and its logs, temporary work, recovery and working data, save backups, operation recovery, and memo backups. The list shows sizes, observed file counts and changes; Details includes exact byte counts, the size-check time and completion state.

Scanning runs in the background after an explicit action. It creates, changes and deletes no files. Original PDFs and other files outside the management directory are excluded; links are not followed. Sizes sum logical file sizes per name, rather than disk allocation. Parent rows contain child rows, so do not add the rows together. The log-directory row includes ZIP copies and inspection logs, separately from the five diagnostic logs' 10 MB budget.

Cancellation, read failures and scan limits show observed results only; uninspected locations are not assumed to be zero. Limits are 100,000 entries, depth 64 and five seconds from scan start, checked between operations. An OS read may take longer. The cancel button or closing the window requests cancellation. Saves during inspection can change the sizes, so they are estimates. Changes are compared only when both scans completed.

Presence and size checks have separate timestamps and cooldowns. Size checks are ten seconds apart; reopening keeps the result and cooldown. Refreshing presence does not scan sizes. Restarting resets size results to unchecked. There is no automatic deletion.

## Information useful for a report

Include reproduction steps, the error message, application version, and whether the affected item is a PDF, note, or annotation. Do not share PDFs or note text that contain personal information.
