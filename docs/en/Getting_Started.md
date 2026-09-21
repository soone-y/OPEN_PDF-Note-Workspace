# Getting Started

## Start the application

Run `pdf_note_workspace.exe` from the distribution folder. Use `readonly_viewer.exe` when you only need to view files. Keep the EXEs, DLLs, and `pdf_workspace_setup.json` together in that folder.

## Workspace

The bundled `sample_workspace` contains one practice PDF and companion note in `01_Lecture_Samples/Session_01_Basics`. `Session_02_Note_Formats` contains one Markdown, TeX, plain-text, CSV, and standard-note example, plus a paired PDF and annotation file for a round-trip note-to-PDF link; it is included in both editions. Sessions 03 and 04 each include a concise `.clro` note. Session 04 contains one Full-edition-only PowerPoint-to-PDF native-chart example and is absent from Lite.

The application does not overwrite original PDFs, notes, or annotations directly. Its normal save flow protects in-progress work and creates a backup before safely saving to the original.

## Full and Lite editions

The Full edition includes local Office-to-PDF conversion through the bundled LibreOffice runtime. The Lite edition omits that runtime, but PDF viewing, annotations, notes, saving, and recovery work the same way.

The first LibreOffice conversion can take longer to start while Windows performs a security check. Conversion runs in the background, so the main app remains usable. Canceling a conversion, closing the app, or a Windows end-session request also stops the LibreOffice conversion processes. Originals are not changed.

We recommend extracting either ZIP into a new writable folder directly under your user profile or Documents. If Windows reports that a target path is too long, you can instead try a short drive-root folder such as `C:\PDFNote`. Avoid deeply nested folders or very long folder names, which can prevent Windows from extracting the Office conversion runtime.

## If something goes wrong

Check `docs/README.md`, `docs/legal/`, and `licenses/` in the distribution.
