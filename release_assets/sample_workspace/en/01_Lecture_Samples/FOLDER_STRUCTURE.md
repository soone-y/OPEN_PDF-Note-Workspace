# Folder Structure

This file sits directly in `01_Lecture_Samples`, so the session list shows it under the "(Direct files)" entry.

A workspace has two levels: lecture, then session. A folder chosen in the lecture list is a lecture, and each folder inside it is a session. PDFs and notes placed in a session folder appear in the PDF and note lists. Files placed directly in a lecture folder, like this one, open from "(Direct files)".

```text
sample_workspace/
├─ 01_Lecture_Samples/                  lecture
│  ├─ FOLDER_STRUCTURE.md               this file (direct file)
│  ├─ Session_01_Basics/                session
│  ├─ Session_02_Note_Formats/          session
│  ├─ Session_03_Early_Design/          session
│  ├─ Session_04_Office_Conversion/     session (Full edition only)
│  └─ Session_05_Math_Display/          session
├─ __pdf_note_workspace__/              app-managed (do not put materials here)
└─ workspace.json                       workspace settings
```

## Sessions

- `Session_01_Basics`: the PDF guide and standard note opened on first launch, for trying viewing, annotation, editing, and saving.
- `Session_02_Note_Formats`: material for directly opening Markdown, TeX source, plain text, CSV, and standard notes by format. Its standard note and `pdf_link_practice.pdf` demonstrate a round-trip link between a note and a PDF.
- `Session_03_Early_Design`: a reference PDF for a historical early-design exercise. It is not the current specification. Includes a `.clro` note.
- `Session_04_Office_Conversion`: one PowerPoint-derived PDF with a native chart, converted by the local LibreOffice runtime, plus a `.clro` note.
- `Session_05_Math_Display`: a `.clro` note on writing formulas and how they are positioned, plus a practice PDF for dragging a formula from the bottom pane's "MathBox input" onto a page.

The Full edition includes Sessions 01 to 05. Lite omits the conversion runtime and Session 04.

## Terms

- Workspace: the folder you choose to hold your materials and notes. This `sample_workspace` is a workspace. Earlier versions labeled it "Root Folder".
- Lecture and session: the two levels inside a workspace. Generic terminology can label these as upper and lower items.
- `__pdf_note_workspace__`: the working-data folder where the app keeps work protection and backups.

Other terms and their full meanings are in the "Terms" section of the bundled Help Reference (`Help_Reference.md`).

## Using your own materials

For your own work, use **File > Choose Workspace...** to select a separate writable workspace. Create a lecture folder in it, then session folders, and place PDFs and notes there. `__pdf_note_workspace__` holds app-managed logs, temporary files, and backups; do not put materials there.
