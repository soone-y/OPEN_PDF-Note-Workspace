# Getting Started

## Start the application

Run `pdf_note_workspace.exe` from the distribution folder. Use `readonly_viewer.exe` when you only need to view files. Keep the EXEs, DLLs, and `pdf_note_workspace_setup.json` together in that folder. A legacy `pdf_workspace_setup.json` is read once and safely copied to the new name; the legacy file is not removed.

## Workspace

The bundled `sample_workspace` contains one practice PDF and companion note in `01_Lecture_Samples/Session_01_Basics`. `Session_02_Note_Formats` contains one Markdown, TeX, plain-text, CSV, and standard-note example, plus a paired PDF and annotation file for a round-trip note-to-PDF link; it is included in both editions. Sessions 03 and 04 each include a concise `.clro` note. Session 04 contains one Full-edition-only PowerPoint-to-PDF native-chart example and is absent from Lite.

The application does not overwrite original PDFs, notes, or annotations directly. Its normal save flow protects in-progress work and creates a backup before safely saving to the original.

## Choose where your data is kept

Use the distributed ZIP by extracting it. Persistent files are kept in the following locations.

| Location | What is kept there |
| --- | --- |
| Extracted application folder | EXEs, DLLs, and setup files such as `pdf_note_workspace_setup.json` and `readonly_setup.json` |
| Workspace you choose | PDFs, notes, and `.clrop` files. `__pdf_note_workspace__` holds automatic work protection, backups, logs, and temporary Office-conversion data. |
| Output destination you choose / external folder registered as a temporary path | Files that you explicitly save, export, or use for external work |

The application does not use AppData, the Windows temporary folder, or the launch working directory as an implicit storage location. If it cannot write to a location you chose, it reports the failure instead of saving automatically somewhere else. This describes persistent file storage, not the memory used while the application is running.

## Full and Lite editions

The Full edition includes local Office-to-PDF conversion through the bundled LibreOffice runtime. The Lite edition omits that runtime, but PDF viewing, annotations, notes, saving, and recovery work the same way.

The first LibreOffice conversion can take longer to start while Windows performs a security check. Conversion runs in the background, so the main app remains usable. Canceling a conversion, closing the app, or a Windows end-session request also stops the LibreOffice conversion processes. Originals are not changed.

We recommend extracting either ZIP into a new writable folder directly under your user profile or Documents. If Windows reports that a target path is too long, you can instead try a short drive-root folder such as `C:\PDFNote`. Avoid deeply nested folders or very long folder names, which can prevent Windows from extracting the Office conversion runtime.

## Update the application

The application does not automatically check for, download, or install updates. To update, open GitHub Releases yourself and download the new ZIP. Do not overwrite the distribution folder you are using or mix files from different packages.

1. Close the application.
2. Extract the new ZIP into a separate new folder and confirm that it starts.

Keep the previous distribution folder and workspace until you have confirmed that the new version starts and that the data you need is present. Your PDFs, notes, and paired `.clrop` files can remain in their existing locations after the update.

## If something goes wrong

Check `docs/README.md`, `docs/legal/`, and `licenses/` in the distribution.
