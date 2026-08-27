# Getting Started

## Start the application

Run `pdf_note_workspace.exe` from the distribution folder. Use `readonly_viewer.exe` when you only need to view files. Keep the EXEs, DLLs, and `pdf_workspace_setup.json` together in that folder.

## Workspace

Use the bundled `sample_workspace` for an initial check. On a first launch, `01_Lecture_Samples/Session_01_Basics` is selected and opens one practice PDF with one companion note, so you can immediately try viewing, editing, and saving. Session 03 contains Full-edition-only Office conversion-result PDFs and is deliberately absent from Lite. For your own work, open a new writable local folder as a workspace. Keeping it separate from the distribution folder protects your work when updating the app.

The application does not overwrite original PDFs, notes, or annotations directly. Its normal save flow, stage data, and backups preserve a recoverable state.

## Full and Lite editions

The Full edition includes local Office-to-PDF conversion through the bundled LibreOffice runtime. The Lite edition omits that runtime, but PDF viewing, annotations, notes, saving, and recovery work the same way.

We recommend extracting either ZIP into a new writable folder directly under your user profile or Documents. If Windows reports that a target path is too long, you can instead try a short drive-root folder such as `C:\PDFNote`. Avoid deeply nested folders or very long folder names, which can prevent Windows from extracting the Office conversion runtime.

## If something goes wrong

Check `docs/README.md`, `docs/legal/`, and `licenses/` in the distribution. If personal work data is stored inside the distribution folder, copy the entire workspace elsewhere before updating or deleting the distribution.
